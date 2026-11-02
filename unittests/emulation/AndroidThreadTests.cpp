//===- AndroidThreadTests.cpp - Independent cooperative pthread checks ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/linux/android/AndroidThreads.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/PhysicalMemory.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <memory>
#include <set>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidThread : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 100000;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 8;
    Options.Android->TraceLimit = 16000;
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 1024});
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
#endif
  }
  ProcessResult run(const char *Entry, uint64_t Mode = 0) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Mode};
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
  uint64_t word(const ProcessResult &R, size_t Index) {
    if (R.MemorySnapshots.size() != 1 ||
        R.MemorySnapshots[0].Bytes.size() < (Index + 1) * 8) {
      ADD_FAILURE() << "missing memory snapshot";
      return UINT64_MAX;
    }
    return llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() +
                                           Index * 8);
  }
  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};
TEST_P(AndroidThread,
       IndependentContextsTLSAndNestedCallbacksShareOnlyProcessState) {
  auto R = run("threads_identity");
  returned(R, 0);
  ASSERT_EQ(R.NativeThreads.size(), 3u);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(word(R, 3), 77u);
  EXPECT_EQ(word(R, 5), 1000u);
  EXPECT_EQ(word(R, 10), 0u);
  EXPECT_EQ(word(R, 12), 1001u);
  EXPECT_EQ(word(R, 13), 1u);
  EXPECT_EQ(word(R, 1), word(R, 2) + 16);
  EXPECT_EQ(word(R, 4), word(R, 2) + 0x200);
  EXPECT_EQ(word(R, 11), R.NativeThreads[0].Handle);
  EXPECT_FALSE(R.NativeThreads[0].Retired);
  std::set<uint64_t> TLS{word(R, 2)};
  for (size_t I = 1; I <= 2; ++I) {
    const auto &T = R.NativeThreads[I];
    const size_t Row = I * 32;
    EXPECT_EQ(T.ID, 1000 + I);
    EXPECT_TRUE(T.Finished);
    EXPECT_TRUE(T.Retired);
    EXPECT_FALSE(T.Detached);
    EXPECT_EQ(word(R, Row), T.Handle);
    EXPECT_EQ(word(R, Row + 10), T.Handle);
    for (unsigned Offset : {1u, 2u, 3u, 13u, 15u})
      EXPECT_EQ(word(R, Row + Offset), T.ID);
    EXPECT_EQ(word(R, Row + 4), 1000u);
    EXPECT_TRUE(TLS.insert(word(R, Row + 5)).second);
    EXPECT_EQ(word(R, Row + 5), T.TLS);
    EXPECT_EQ(word(R, Row + 6), T.TLS + 16);
    EXPECT_EQ(word(R, Row + 7), 0u);
    EXPECT_EQ(word(R, Row + 8), 100 + I);
    EXPECT_EQ(word(R, Row + 11), T.TLS + 0x200);
    EXPECT_EQ(word(R, Row + 12), 0u);
    EXPECT_EQ(word(R, Row + 16), 0u);
    EXPECT_EQ(word(R, Row + 14), 2u);
    EXPECT_EQ(word(R, Row + 20), T.StackBase);
    EXPECT_EQ(word(R, Row + 21), T.StackSize);
    EXPECT_EQ(word(R, Row + 22), T.GuardSize);
    EXPECT_GE(word(R, Row + 9), T.StackBase + T.GuardSize);
    EXPECT_LT(word(R, Row + 9), T.StackBase + T.StackSize);
    EXPECT_EQ(word(R, Row + 23), 0xfeed0000 + I);
    EXPECT_EQ(word(R, Row + 24), 0x0123456789abcdefUL + I);
    EXPECT_EQ(word(R, Row + 25), 0x0123456789abcdefUL + I);
    EXPECT_EQ(word(R, Row + 26), I << 22);
    EXPECT_EQ(word(R, Row + 27), I);
    EXPECT_EQ(word(R, Row + 28), I == 1 ? 1u : 0u);
    EXPECT_EQ(word(R, 7 + I), 0xfedc000000000000UL + I);
  }
  ASSERT_GT(R.TraceThreads.size(), 8u);
  EXPECT_EQ(R.TraceThreads.front().Index, 0u);
  for (size_t I = 1; I < R.TraceThreads.size(); ++I) {
    EXPECT_GT(R.TraceThreads[I].Index, R.TraceThreads[I - 1].Index);
    EXPECT_NE(R.TraceThreads[I].ID, R.TraceThreads[I - 1].ID);
    EXPECT_LT(R.TraceThreads[I].Index, R.Trace.size());
  }
  for (const auto &C : R.NativeCalls) {
    ASSERT_TRUE(C.ThreadID);
    EXPECT_GE(*C.ThreadID, 1000u);
    EXPECT_LE(*C.ThreadID, 1002u);
    EXPECT_TRUE(C.Result) << C.Name;
  }
  for (const auto &S : R.Services) {
    ASSERT_TRUE(S.ThreadID);
    EXPECT_EQ(S.Result, S.ThreadID);
  }
  auto Again = run("threads_identity");
  EXPECT_EQ(processResultJSON(R), processResultJSON(Again));
}
TEST_P(AndroidThread, EntryReturnAndDetachedCompletionAreSeparateBoundaries) {
  Options.InstructionQuantum = 100000;
  for (uint64_t Mode : {0u, 1u}) {
    Options.Android->DrainThreads = false;
    auto R = run("threads_detached", Mode);
    returned(R, 73);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[0].Finished);
    EXPECT_FALSE(R.NativeThreads[1].Finished);
    EXPECT_TRUE(R.NativeThreads[1].Detached);
    EXPECT_EQ(word(R, 2), 0u);
    EXPECT_EQ(word(R, 4), 22u);
    EXPECT_EQ(word(R, 5), 22u);
    Options.Android->DrainThreads = true;
    R = run("threads_detached", Mode);
    returned(R, 73);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[1].Finished);
    EXPECT_TRUE(R.NativeThreads[1].Retired);
    EXPECT_EQ(word(R, 2), 1001u);
  }
}
TEST_P(AndroidThread, NiceStateInheritsAtCreationAndEndsWithTheChildTask) {
  Options.LinuxPriority.emplace().Tasks = {{1000, -4}};
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP, ExecutionBackendKind::HVF}) {
    SCOPED_TRACE(unsigned(Backend));
    ExecutionConfiguration Configuration;
    Configuration.Backend = Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      ASSERT_FALSE(requireHvf(Backend, GuestArchitecture::AArch64))
          << Probe->Reason;
      RecordProperty("unavailable_transport_" +
                         std::to_string(unsigned(Backend)),
                     Probe->Reason);
      continue;
    }
    Options.Backend = Backend;
    auto R = run("threads_priority");
    returned(R, 0);
    ASSERT_EQ(R.NativeThreads.size(), 3u);
    const uint64_t Parent[] = {
        0,  17,         0, 15, 1000, 0, 0, uint64_t(0) - 3, uint64_t(0) - 3,
        15, UINT64_MAX, 3};
    const uint64_t Child[] = {1001, 17, 0, 11, UINT64_MAX,     13,
                              11,   0,  0, 11, uint64_t(0) - 3};
    const uint64_t Grandchild[] = {1002, 11, 0, 8};
    for (unsigned I = 0; I < std::size(Parent); ++I)
      EXPECT_EQ(word(R, I), Parent[I]) << I;
    for (unsigned I = 0; I < std::size(Child); ++I)
      EXPECT_EQ(word(R, 32 + I), Child[I]) << I;
    for (unsigned I = 0; I < std::size(Grandchild); ++I)
      EXPECT_EQ(word(R, 64 + I), Grandchild[I]) << I;
  }
}
TEST_P(AndroidThread, ForeignTasksCoexistWithLiveAndExitedChildren) {
  Options.LinuxFiles.emplace();
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
  Options.LinuxTime.emplace().Clocks = {{2, {7, 9}}, {-16006, {11, 13}}};
  for (auto Backend :
       {ExecutionBackendKind::Unicorn, ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      ASSERT_FALSE(requireHvf(Backend, GuestArchitecture::AArch64))
          << Probe->Reason;
      RecordProperty("unavailable_transport_" +
                         std::to_string(unsigned(Backend)),
                     Probe->Reason);
      continue;
    }
    Options.Backend = Backend;
    for (auto GKI :
         {AndroidGKIKernel::Android12_5_10, AndroidGKIKernel::Android17_6_18}) {
      Kernel.GKI = GKI;
      const uint64_t NonLeader =
          GKI == AndroidGKIKernel::Android17_6_18 ? 2 : 22;
      auto R = run("threads_task_catalogue");
      returned(R, 0);
      std::array<uint64_t, 128> Expected{};
      Expected[0] = Expected[6] = 1;
      Expected[1] = Expected[9] = Expected[11] = Expected[20] = 3;
      Expected[3] = Expected[17] = uint64_t(0) - NonLeader;
      Expected[4] = Expected[5] = Expected[8] = uint64_t(0) - 3;
      Expected[13] = 1000;
      Expected[16] = 1001;
      Expected[14] = Expected[18] = UINT64_MAX;
      Expected[15] = 22;
      Expected[19] = NonLeader;
      Expected[24] = 7;
      Expected[25] = 9;
      Expected[27] = 11;
      Expected[28] = 13;
      Expected[30] = uint64_t(0) - 22;
      for (unsigned I = 0; I < Expected.size(); ++I)
        EXPECT_EQ(word(R, I), Expected[I]) << I;
      ASSERT_EQ(R.NativeThreads.size(), 2u);
      EXPECT_TRUE(R.NativeThreads[1].Retired);
      if (GKI == AndroidGKIKernel::Android17_6_18)
        for (unsigned Mode : {1, 2}) {
          R = run("threads_task_catalogue", Mode);
          EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService)
              << R.Diagnostic;
          EXPECT_NE(R.Diagnostic.find("guest-thread descriptor lifetime"),
                    std::string::npos);
          ASSERT_FALSE(R.Services.empty());
          EXPECT_EQ(R.Services.back().Number, 434u);
          EXPECT_FALSE(R.Services.back().Result);
          EXPECT_EQ(word(R, 6), 0u);
        }
    }
  }
}
TEST_P(AndroidThread, ForeignCatalogueCannotPreclaimFutureChildIdentities) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.GKI = AndroidGKIKernel::Android12_5_10;
  Kernel.Tasks.emplace();
  returned(run("threads_identity"), 0);
  for (uint32_t ID : {1001, 1007}) {
    Kernel.Tasks->emplace(ID, LinuxKernelTask{true});
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), linux_model::KernelOptions);
    Kernel.Tasks->clear();
  }
  Kernel.Tasks->emplace(1008, LinuxKernelTask{true});
  returned(run("threads_identity"), 0);
}

TEST_P(AndroidThread, NiceStateWithoutParentInputsRemainsUnmodeled) {
  for (bool Present : {false, true}) {
    if (Present)
      Options.LinuxPriority.emplace();
    auto R = run("threads_priority", 1);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 141u);
    EXPECT_FALSE(R.Services.back().Result);
  }
}
TEST_P(AndroidThread, ThreadAndProcessExitKeepDifferentOwnership) {
  for (uint64_t Mode : {0u, 1u}) {
    auto R = run("threads_last_exit", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, Mode ? 7u : 8u);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_TRUE(R.NativeThreads[0].Finished);
  }
  for (uint64_t Mode : {0u, 1u, 2u, 3u, 4u}) {
    auto R = run("threads_exit", Mode);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    if (Mode < 3) {
      returned(R, 74);
      EXPECT_EQ(word(R, 3), Mode ? 0u : 0xfedcba9876543210UL);
      EXPECT_EQ(word(R, 4), 1u);
      EXPECT_TRUE(R.NativeThreads[1].Retired);
    } else {
      EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, Mode == 3 ? 37u : 38u);
      EXPECT_FALSE(R.ReturnValue);
      EXPECT_EQ(word(R, 4), 0u);
      EXPECT_FALSE(R.NativeThreads[0].Finished);
    }
  }
}
TEST_P(AndroidThread, StartRoutineReturnAndExplicitExitShareThreadCompletion) {
  Options.Android->DrainThreads = true;
  for (uint64_t Mode : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_terminal_return", Mode);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 80u);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 2), 0u);
    EXPECT_EQ(word(R, 3), Mode & 2 ? 0u : 0xfedcba9876543210UL);
    EXPECT_TRUE(R.NativeThreads[1].Finished);
    EXPECT_TRUE(R.NativeThreads[1].Retired);
    EXPECT_EQ(R.NativeThreads[1].Detached, bool(Mode & 2));
    EXPECT_EQ(R.NativeThreads[1].ReturnValue, 0xfedcba9876543210UL);
  }
}
TEST_P(AndroidThread, EntryAndCallbackReturnsStillRequireTheirCallerStack) {
  for (bool Drain : {false, true}) {
    SCOPED_TRACE(Drain);
    Options.Android->DrainThreads = Drain;
    auto R = run("threads_return_with_stack_residue");
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(android_model::diagnostic::ThreadStack),
              std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_FALSE(R.NativeThreads[0].Finished);
    R = run("threads_callback_stack");
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(android_model::diagnostic::CallbackStack),
              std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_FALSE(R.NativeThreads[0].Finished);
  }
}
TEST_P(AndroidThread, TerminalReturnsAgreeWithNativeTransport) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::HVF;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  Options.Android->DrainThreads = true;
  for (uint64_t Mode : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(Mode);
    Options.Backend = ExecutionBackendKind::Unicorn;
    auto Software = run("threads_terminal_return", Mode);
    ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
        << Software.Diagnostic;
    Options.Backend = ExecutionBackendKind::HVF;
    auto Native = run("threads_terminal_return", Mode);
    ASSERT_EQ(Native.Stop, ProcessStopReason::Returned) << Native.Diagnostic;
    Native.SelectedBackend = Software.SelectedBackend;
    Native.BackendSelectionReason = Software.BackendSelectionReason;
    EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
  }
}
TEST_P(AndroidThread,
       CreationRefusalPreservesErrnoAndDoesNotPublishAnIdentity) {
  Options.Android->ThreadLimit = 2;
  auto R = run("threads_limits");
  returned(R, 0);
  EXPECT_EQ(word(R, 4), 11u);
  EXPECT_EQ(word(R, 5), 0u);
  EXPECT_EQ(word(R, 6), 91u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  for (uint64_t Mode : {1u, 4u}) {
    R = run("threads_limits", Mode);
    returned(R, 0);
    EXPECT_EQ(word(R, 0), 11u);
    EXPECT_EQ(word(R, 1), 0x12345678u);
    EXPECT_EQ(word(R, 6), 91u);
    EXPECT_EQ(R.NativeThreads.size(), 1u);
  }
  for (uint64_t Mode : {2u, 3u, 5u}) {
    R = run("threads_limits", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_EQ(word(R, 1), 0x12345678u);
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidThread, BadHandlesAndEntryPointersStopBeforeChildPublication) {
  returned(run("threads_invalid", 0), 35);
  returned(run("threads_invalid", 1), 3);
  for (uint64_t Mode : {2u, 3u, 4u, 5u, 6u}) {
    auto R = run("threads_invalid", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidThread,
       JoinCyclesStopAndCompletedUnjoinedThreadsRemainQueryable) {
  auto R = run("threads_join_cycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[0].Waiting);
  EXPECT_TRUE(R.NativeThreads[1].Waiting);
  EXPECT_FALSE(R.ReturnValue);
  for (uint64_t Mode : {0u, 1u}) {
    R = run("threads_join_finished", Mode);
    returned(R, 0);
    EXPECT_EQ(word(R, 3), 0u);
    EXPECT_TRUE(R.NativeThreads[1].Retired);
    if (!Mode)
      EXPECT_EQ(word(R, 4), 0x1234567800000042UL);
  }
}
TEST_P(AndroidThread, DynamicNamesAndProviderLifetimeUseTheSameThreadModel) {
  Options.Android->Libraries["libthread-model.so"] = {"pthread_create",
                                                      "pthread_join"};
  auto R = run("threads_dynamic");
  returned(R, 75);
  EXPECT_EQ(word(R, 4), 0x1234567800000042UL);
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "pthread_create" || Call.Name == "pthread_join") {
      EXPECT_EQ(Call.Library, "libthread-model.so");
      EXPECT_EQ(Call.ThreadID, 1000u);
      EXPECT_EQ(Call.Result, 0u);
    }
  R = run("threads_dynamic", 1);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.NativeThreads.size(), 1u);
}
TEST_P(AndroidThread, NullTIDQueryReturnsMinusOneWithoutChangingErrno) {
  Options.Android->Libraries["libthread-model.so"] = {"pthread_gettid_np"};
  for (uint64_t Mode : {0u, 1u}) {
    auto R = run("threads_null_tid", Mode);
    returned(R, 0);
    EXPECT_EQ(word(R, 0), uint64_t(UINT32_MAX));
    EXPECT_EQ(word(R, 1), 91u);
    unsigned Queries = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_gettid_np") {
        ++Queries;
        EXPECT_EQ(Call.Arguments[0], 0u);
        EXPECT_EQ(Call.Result, uint64_t(UINT32_MAX));
        EXPECT_EQ(Call.Library, Mode ? "libthread-model.so" : "");
      }
    EXPECT_EQ(Queries, 1u);
  }
}
TEST_P(AndroidThread, LimitsRemainSharedAcrossSuspensionsAndTraceAttribution) {
  Options.Limits.Instructions = 300;
  auto R = run("threads_identity");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 300u);
  EXPECT_FALSE(R.ReturnValue);
  Options.Limits.Instructions = 100000;
  Options.OutputLimit = Options.Android->TraceLimit * 8 +
                        Options.Android->ThreadLimit * 128 + 1024 + 16;
  R = run("threads_identity");
  returned(R, 0);
  EXPECT_TRUE(R.TraceTruncated);
  EXPECT_EQ(R.TraceThreads.size(), 1u);
  EXPECT_LT(R.Trace.size(), Options.Android->TraceLimit);
}
TEST_P(AndroidThread, GuardPagesCallbackExitAndCorruptTLSRemainExplicitStops) {
  auto R = run("threads_boundary", 0);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  ASSERT_TRUE(R.LastCPUExit && R.LastCPUExit->Fault);
  EXPECT_EQ(R.LastCPUExit->Fault->Address, R.NativeThreads[1].StackBase);
  EXPECT_FALSE(R.NativeThreads[1].Finished);
  R = run("threads_boundary", 1);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.NativeThreads[1].Finished);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("threads_boundary", 2);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("TLS identity"), std::string::npos);
  EXPECT_FALSE(R.NativeThreads[1].Finished);
}
TEST_P(AndroidThread, OnceWaitersObserveOneInitializerAndPreserveContexts) {
  for (uint64_t Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    if (Mode == 2)
      Options.Android->Libraries["libc.so"] = {"pthread_once"};
    auto R = run("threads_once_wait", Mode);
    returned(R, 0);
    ASSERT_EQ(R.NativeThreads.size(), 4u);
    EXPECT_EQ(word(R, 1), 1u);
    EXPECT_EQ(word(R, 3), 1u);
    EXPECT_EQ(word(R, 4), 0x123456789abcdef0ULL);
    EXPECT_EQ(word(R, 5), 0xfedcba9876543210ULL);
    EXPECT_EQ(word(R, 6), 0u);
    EXPECT_EQ(word(R, 7), 0u);
    EXPECT_EQ(word(R, 8), 2u);
    EXPECT_EQ(word(R, 9), 2u);
    std::set<uint64_t> Callers;
    unsigned OnceCalls = 0;
    bool Named = false;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "pthread_once") {
        ++OnceCalls;
        EXPECT_EQ(Call.Result, 0u);
        ASSERT_TRUE(Call.ThreadID);
        Callers.insert(*Call.ThreadID);
      }
      Named |= Call.Name == "dlsym" && Call.Symbol == "pthread_once" &&
               Call.Library == "libc.so";
    }
    EXPECT_EQ(OnceCalls, 6u);
    EXPECT_EQ(Callers, (std::set<uint64_t>{1000, 1001, 1002, 1003}));
    EXPECT_EQ(Named, Mode == 2);
    for (size_t I = 1; I <= 3; ++I) {
      const size_t Row = I * 32;
      const uint64_t Cookie = 0xfeedface0000ULL + I;
      EXPECT_EQ(word(R, Row + 1), 0u);
      EXPECT_EQ(word(R, Row + 2), Mode == 1 && I == 2 ? 0x123456789abcdef0ULL
                                                      : 0xfedcba9876543210ULL);
      for (size_t Offset : {3u, 8u, 9u})
        EXPECT_EQ(word(R, Row + Offset), Cookie);
      EXPECT_EQ(word(R, Row + 4), 70u + I);
      EXPECT_EQ(word(R, Row + 5), 1000u + I);
      EXPECT_EQ(word(R, Row + 6), R.NativeThreads[I].TLS);
      EXPECT_EQ(word(R, Row + 7), 1u);
      EXPECT_TRUE(R.NativeThreads[I].Retired);
      EXPECT_FALSE(R.NativeThreads[I].Waiting);
    }
  }
}
TEST_P(AndroidThread, OnceWaitsRejectUnknownRecursiveAndReinitializedControls) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  for (uint64_t Mode : {0u, 1u, 8u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_NE(R.Diagnostic.find("pthread_once"), std::string::npos);
    EXPECT_EQ(word(R, 7), 0u);
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once")
        EXPECT_FALSE(Call.Result);
  }
}
TEST_P(AndroidThread, OnceAndJoinCyclesKeepAllWaitsIncomplete) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  for (uint64_t Mode : {2u, 3u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("pending waits"), std::string::npos);
    ASSERT_EQ(R.NativeThreads.size(), Mode == 2 ? 2u : 3u);
    for (const auto &Thread : R.NativeThreads) {
      EXPECT_TRUE(Thread.Waiting);
      EXPECT_FALSE(Thread.Finished);
    }
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once" || Call.Name == "pthread_join")
        EXPECT_FALSE(Call.Result);
  }
}
TEST_P(AndroidThread, OnceWakeupsRecheckGuestMemoryBeforeCompletingWaiters) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  // Service boundaries order the release, callback return and third-thread
  // mutation without preempting the few instructions before each import.
  Options.InstructionQuantum = 1000;
  for (uint64_t Mode : {4u, 5u, 6u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    unsigned Completed = 0, Pending = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once") {
        Completed += Call.Result.has_value();
        Pending += !Call.Result.has_value();
      }
    EXPECT_EQ(Completed, Mode == 4 ? 0u : 1u);
    EXPECT_EQ(Pending, Mode == 4 ? 2u : 1u);
    if (Mode == 5)
      EXPECT_NE(R.Diagnostic.find("completion changed"), std::string::npos);
  }
}
TEST_P(AndroidThread, OnceWaitingDoesNotCompleteCallsOrResetTheBudget) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  Options.Limits.Instructions = 2000;
  auto R = run("threads_once_failure", 7);
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 2000u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[0].Waiting);
  EXPECT_FALSE(R.ReturnValue);
  unsigned Pending = 0;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "pthread_once") {
      EXPECT_FALSE(Call.Result);
      ++Pending;
    }
  EXPECT_EQ(Pending, 2u);
}
TEST_P(AndroidThread, OnceWaitsAgreeWithNativeTransport) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::HVF;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Software = run("threads_once_wait", 1);
  returned(Software, 0);
  Options.Backend = ExecutionBackendKind::HVF;
  auto Native = run("threads_once_wait", 1);
  returned(Native, 0);
  EXPECT_EQ(Native.SelectedBackend, ExecutionBackendKind::HVF);
  Native.SelectedBackend = Software.SelectedBackend;
  Native.BackendSelectionReason = Software.BackendSelectionReason;
  EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
}
TEST_P(AndroidThread, AvailableNativeTransportAgreesWithSoftwareContexts) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::HVF;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Software = run("threads_identity");
  returned(Software, 0);
  Options.Backend = ExecutionBackendKind::HVF;
  auto Native = run("threads_identity");
  returned(Native, 0);
  EXPECT_EQ(Native.SelectedBackend, ExecutionBackendKind::HVF);
  Native.SelectedBackend = Software.SelectedBackend;
  Native.BackendSelectionReason = Software.BackendSelectionReason;
  EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidThread,
                         testing::Values("threads-O0-none",
                                         "threads-O0-android",
                                         "threads-O0-relr", "threads-O2-none",
                                         "threads-O2-android",
                                         "threads-O2-relr"));
TEST(AndroidThreadMemory, CapacityRefusalKeepsInputsAndGenericMappingFailures) {
  constexpr uint64_t Page = 4096, MemoryLimit = 4 * 1024 * 1024;
  // Default 0xfc000-byte stack, one guard page and one TLS page.
  constexpr uint64_t ChildBytes = 0xfe000;
  constexpr uint64_t Sentinel = 0x123456789abcdef0ULL;
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  for (bool Fragmented : {true, false}) {
    SCOPED_TRACE(Fragmented);
    auto Backend = createExecutionBackend(Configuration, MemoryLimit);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    auto &CPU = *Backend->CPU;
    auto &Space = *CPU.addressSpace();
    auto RAM = Space.physicalMemory();
    ASSERT_EQ(llvm::toString(Space.map(
                  Buffer, Page, Read | Write | Execute | UserAccessible)),
              "");
    ASSERT_EQ(llvm::toString(Space.map(Buffer + Page, Page,
                                       Read | Write | UserAccessible)),
              "");
    ASSERT_EQ(llvm::toString(Space.map(android_model::TLSAddress, Page,
                                       Read | Write | UserAccessible)),
              "");
    ASSERT_EQ(llvm::toString(CPU.writeInteger(Buffer, Sentinel, 8)), "");
    ASSERT_EQ(llvm::toString(CPU.writeInteger(android_model::TLSAddress,
                                              android_model::TLSAddress, 8)),
              "");
    ASSERT_EQ(
        llvm::toString(CPU.writeInteger(android_model::ErrnoAddress, 91, 4)),
        "");
    ASSERT_EQ(llvm::toString(CPU.writeRegister(CPURegister::AArch64TPIDR_EL0,
                                               {android_model::TLSAddress, 0})),
              "");
    ProcessOptions Options;
    Options.MemoryLimit = MemoryLimit;
    Options.StackSize = Page;
    Options.Android.emplace();
    Options.Android->ThreadLimit = 2;
    ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                         GuestArchitecture::AArch64,
                         ExecutionBackendKind::Unicorn,
                         {}};
    auto Calls = IntegerABI::get(IntegerCallingConvention::AAPCS64);
    ASSERT_TRUE(bool(Calls)) << llvm::toString(Calls.takeError());
    const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, Page};
    linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 2 * Page, Options,
                                      Result);
    android_model::GuestThreads Threads(CPU, *Calls, Options, Result, Kernel);
    ASSERT_EQ(llvm::toString(Threads.initialize(Buffer + Page)), "");

    std::vector<std::shared_ptr<MemoryRegion>> Owners;
    if (Fragmented) {
      // Fill every remaining physical page, then free alternate allocation
      // owners. Total capacity is ample, but no free span exceeds one page.
      while (RAM->allocatedBytes() < MemoryLimit) {
        auto Owner = RAM->allocate(Page);
        ASSERT_TRUE(bool(Owner)) << llvm::toString(Owner.takeError());
        Owners.push_back(std::move(*Owner));
      }
      for (size_t I = 0; I < Owners.size(); I += 2)
        Owners[I].reset();
      ASSERT_GE(MemoryLimit - RAM->allocatedBytes(), ChildBytes);
      auto Blocked = RAM->allocate(ChildBytes);
      ASSERT_FALSE(bool(Blocked));
      auto E = Blocked.takeError();
      EXPECT_TRUE(E.isA<GuestMemoryLimitError>());
      llvm::consumeError(std::move(E));
    } else {
      // A slot collision is a generic mapping error, not capacity exhaustion.
      ASSERT_EQ(llvm::toString(
                    Space.map(android_model::thread_model::ArenaBase + Page,
                              Page, Read | Write | UserAccessible)),
                "");
    }
    ASSERT_GE(MemoryLimit - Space.mappedBytes(), ChildBytes);
    const uint64_t Mapped = Space.mappedBytes(),
                   Allocated = RAM->allocatedBytes(),
                   Generation = Space.mappingGeneration();
    auto Budget = ExecutionBudget::create(Options.Limits);
    ASSERT_TRUE(bool(Budget)) << llvm::toString(Budget.takeError());
    const android_model::LinkedImage Linked{};
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, **Budget,
                                Linked, &Threads);
    NativeCallEvent Call{};
    Call.Name = "pthread_create";
    Call.Arguments[0] = Buffer;
    // No guest code executes: creation must refuse before preparing a context.
    Call.Arguments[2] = Buffer + 64;
    auto Value = Model.invoke(Call);
    if (Fragmented) {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(Value->has_value());
      ASSERT_TRUE(std::holds_alternative<uint64_t>(**Value));
      EXPECT_EQ(std::get<uint64_t>(**Value), 11u);
    } else {
      ASSERT_FALSE(bool(Value));
      auto E = Value.takeError();
      EXPECT_FALSE(E.isA<GuestMemoryLimitError>());
      llvm::consumeError(std::move(E));
    }
    EXPECT_EQ(Space.mappedBytes(), Mapped);
    EXPECT_EQ(RAM->allocatedBytes(), Allocated);
    EXPECT_EQ(Space.mappingGeneration(), Generation);
    auto Handle = CPU.readInteger(Buffer, 8);
    ASSERT_TRUE(bool(Handle)) << llvm::toString(Handle.takeError());
    EXPECT_EQ(*Handle, Sentinel);
    auto Errno = CPU.readInteger(android_model::ErrnoAddress, 4);
    ASSERT_TRUE(bool(Errno)) << llvm::toString(Errno.takeError());
    EXPECT_EQ(*Errno, 91u);
    Threads.report();
    ASSERT_EQ(Result.NativeThreads.size(), 1u);
    EXPECT_FALSE(Result.NativeThreads.front().Finished);
  }
}
TEST(AndroidThreadOptions, RejectsWrongTypesAndInvalidSchedulingLimits) {
  for (const char *JSON : {R"({"android":{"thread_limit":true}})",
                           R"({"android":{"thread_limit":-1}})",
                           R"({"android":{"drain_threads":1}})"}) {
    auto O = processOptionsFromJSON(JSON);
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  for (auto [Limit, Drain] :
       {std::pair<uint64_t, bool>{0, false}, {257, false}, {1, true}}) {
    ProcessOptions O;
    O.Android.emplace();
    O.Android->EntrySymbol = "entry";
    O.Android->ThreadLimit = Limit;
    O.Android->DrainThreads = Drain;
    auto R = emulateProcess("absent-thread-input.so",
                            ProcessProfile::AndroidNativeAArch64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("thread_limit"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation

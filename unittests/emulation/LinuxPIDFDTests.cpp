//===- LinuxPIDFDTests.cpp - Released GKI process descriptors ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxKernelAvailability.h"
#include "os/linux/kernel/LinuxTime.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

namespace neverd::emulation {
namespace {
using namespace linux_model;
struct KernelCase {
  AndroidGKIKernel Kernel;
  const char *Label;
  bool ThreadFlag, SingleBuffer;
  uint32_t NonLeader;
};
constexpr KernelCase Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  {AndroidGKIKernel::Name, Label, ThreadFlag, SingleBuffer, Error},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
};
TEST(LinuxPIDFD, VersionInputsRejectContradictionsBeforeLoading) {
  for (const auto &K : Kernels) {
    auto O = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"gki\":\"") + K.Label + "\"}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->LinuxKernel);
    EXPECT_EQ(O->LinuxKernel->GKI, K.Kernel);
    EXPECT_EQ(gkiPidFDFlags(K.Kernel), K.ThreadFlag ? 0x880u : 0x800u);
    O->LinuxKernel->UnavailableSyscalls.insert(
        LinuxUnavailableSyscall::PidFDOpen);
    auto R =
        emulateProcess("missing.elf", ProcessProfile::AndroidNativeAArch64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), KernelOptions);
  }
  for (const char *Value :
       {R"({"gki":null})", R"({"gki":510})", R"({"gki":"android-mainline"})",
        R"({"gki":"android17-6.18-rc1"})",
        R"({"gki":"android12-5.10","unavailable_syscalls":["pidfd_open"]})"}) {
    auto O =
        processOptionsFromJSON(std::string("{\"linux_kernel\":") + Value + "}");
    ASSERT_FALSE(bool(O)) << Value;
    llvm::consumeError(O.takeError());
  }
  ProcessOptions O;
  O.LinuxKernel.emplace().GKI = static_cast<AndroidGKIKernel>(99);
  auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(R));
  EXPECT_EQ(llvm::toString(R.takeError()), KernelOptions);
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class LinuxPIDFDProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld process fixtures unavailable";
#else
    Options.Backend = GetParam().Backend;
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GetParam().ISA;
    C.Contract = GetParam().ISA == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(GetParam().Backend, GetParam().ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    auto &Files = Options.LinuxFiles.emplace();
    Files.DescriptorLimit = 5;
    Files.Files["/catalog/input"] = {0xa5, 0x5a};
#endif
  }
  ProcessResult run(char Mode, const char *Opt, uint32_t Truth = 0) {
    Options.Arguments = {"pidfd", std::string(1, Mode), std::to_string(Truth)};
#ifdef NEVERD_PROCESS_FIXTURE_DIR
    auto Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
        (std::string(GetParam().ISA == GuestArchitecture::X64 ? "X64"
                                                              : "AArch64") +
         "-pidfd-" + Opt + ".elf");
#else
    std::filesystem::path Path;
#endif
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
};
TEST_P(LinuxPIDFDProcess,
       EveryReleasedKernelSharesDescriptorOwnershipAndErrors) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    for (const char *Opt : {"O0", "O2"}) {
      auto R = run('l', Opt);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      ASSERT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxPIDFDProcess, ThreadFlagAndDescriptorLimitsFollowSelectedKernel) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    for (const char *Opt : {"O0", "O2"}) {
      Options.LinuxFiles->DescriptorLimit = 5;
      auto T = run('t', Opt, K.ThreadFlag);
      ASSERT_EQ(T.Stop, ProcessStopReason::Exited) << T.Diagnostic;
      EXPECT_EQ(T.ExitStatus, 0u);
      Options.LinuxFiles->DescriptorLimit = 3;
      auto D = run('d', Opt);
      ASSERT_EQ(D.Stop, ProcessStopReason::Exited) << D.Diagnostic;
      EXPECT_EQ(D.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxPIDFDProcess, VectorImportPreservesReleasedKernelErrorOrder) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    for (const char *Opt : {"O0", "O2"}) {
      SCOPED_TRACE(Opt);
      auto V = run('v', Opt, K.SingleBuffer);
      ASSERT_EQ(V.Stop, ProcessStopReason::Exited) << V.Diagnostic;
      EXPECT_EQ(V.ExitStatus, 0u);
      EXPECT_TRUE(V.StandardOutput.empty());
      EXPECT_TRUE(V.StandardError.empty());

      Options.OutputLimit = 1;
      auto B = run('b', Opt);
      if (K.SingleBuffer)
        EXPECT_EQ(B.Stop, ProcessStopReason::OutputLimit) << B.Diagnostic;
      else {
        ASSERT_EQ(B.Stop, ProcessStopReason::Exited) << B.Diagnostic;
        EXPECT_EQ(B.ExitStatus, 0u);
      }
      EXPECT_TRUE(B.StandardOutput.empty());
      EXPECT_TRUE(B.StandardError.empty());
      Options.OutputLimit = 1024;
    }
  }
  // Missing kernel selection retains the established single-buffer policy.
  Options.LinuxKernel.reset();
  Options.OutputLimit = 1;
  EXPECT_EQ(run('b', "O2").Stop, ProcessStopReason::OutputLimit);
}
TEST_P(LinuxPIDFDProcess, UnknownTargetsAndMissingObservationsStayUnsupported) {
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android17_6_18;
  auto U = run('u', "O2");
  EXPECT_EQ(U.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(U.Diagnostic, PidFDTargetMissing);
  Options.LinuxFiles.reset();
  auto F = run('m', "O2");
  EXPECT_EQ(F.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(F.Diagnostic, FileInputsMissing);
  Options.LinuxFiles.emplace();
  Options.LinuxKernel.reset();
  auto K = run('m', "O2");
  EXPECT_EQ(K.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(K.Services.empty());
  EXPECT_FALSE(K.Services.back().Result);
}
TEST_P(LinuxPIDFDProcess, ClosedTaskCatalogueRetainsLookupAndReservationOrder) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    auto &Kernel = Options.LinuxKernel.emplace();
    Kernel.GKI = K.Kernel;
    Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
    Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    for (const char *Opt : {"O0", "O2"}) {
      auto R =
          run('c', Opt, uint32_t(K.ThreadFlag) | (K.NonLeader == 2 ? 2 : 0));
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
      EXPECT_TRUE(R.StandardOutput.empty());
      EXPECT_TRUE(R.StandardError.empty());
      // An explicitly empty catalogue still includes the running group leader.
      Kernel.Tasks->clear();
      R = run('e', Opt);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
      Kernel.Tasks->emplace(2000, LinuxKernelTask{true});
      Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    }
  }
}
TEST(LinuxPIDFD, ClosedTaskCatalogueInputsRejectAmbiguityBeforeLoading) {
  auto Good = processOptionsFromJSON(
      R"({"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_TRUE(Good->LinuxKernel->Tasks);
  EXPECT_TRUE(Good->LinuxKernel->Tasks->at(2000).GroupLeader);
  EXPECT_FALSE(Good->LinuxKernel->Tasks->at(3000).GroupLeader);
  auto Empty = processOptionsFromJSON(
      R"({"linux_kernel":{"gki":"android12-5.10","tasks":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->LinuxKernel->Tasks);
  EXPECT_TRUE(Empty->LinuxKernel->Tasks->empty());
  for (const char *Tasks :
       {"null", "{}", R"([{"id":1}])", R"([{"group_leader":true}])",
        R"([{"id":0,"group_leader":true}])",
        R"([{"id":-1,"group_leader":true}])",
        R"([{"id":2147483648,"group_leader":true}])",
        R"([{"id":1000,"group_leader":false}])",
        R"([{"id":1,"group_leader":0}])",
        R"([{"id":1,"group_leader":true,"extra":0}])",
        R"([{"id":1,"group_leader":true},{"id":1,"group_leader":false}])"}) {
    auto Bad = processOptionsFromJSON(
        std::string(
            "{\"linux_kernel\":{\"gki\":\"android17-6.18\",\"tasks\":") +
        Tasks + "}}");
    EXPECT_FALSE(bool(Bad)) << Tasks;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
  auto WithoutGKI = processOptionsFromJSON(R"({"linux_kernel":{"tasks":[]}})");
  ASSERT_TRUE(bool(WithoutGKI)) << llvm::toString(WithoutGKI.takeError());
  EXPECT_FALSE(WithoutGKI->LinuxKernel->GKI);
  ASSERT_TRUE(WithoutGKI->LinuxKernel->Tasks);
  EXPECT_TRUE(WithoutGKI->LinuxKernel->Tasks->empty());
  ProcessOptions O;
  auto &K = O.LinuxKernel.emplace();
  K.GKI = AndroidGKIKernel::Android17_6_18;
  K.Tasks.emplace().emplace(1000, LinuxKernelTask{false});
  auto Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
  K.Tasks->clear();
  for (uint32_t I = 1; I <= 4097; ++I)
    K.Tasks->emplace(I, LinuxKernelTask{true});
  Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
  K.Tasks->clear();
  O.LinuxPriority.emplace().Tasks.emplace(2000, 0);
  Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
  O.LinuxPriority.reset();
  O.Android.emplace().ThreadLimit = 2;
  K.Tasks->emplace(1001, LinuxKernelTask{true});
  Bad = emulateProcess("missing.elf", ProcessProfile::AndroidNativeAArch64, O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
}
TEST(LinuxPIDFD, IndependentTaskCatalogueRetainsTypedAndWireValidation) {
  for (const char *Tasks :
       {"null", "{}", R"([{"id":0,"group_leader":true}])",
        R"([{"id":2147483648,"group_leader":true}])",
        R"([{"id":1000,"group_leader":false}])",
        R"([{"id":1,"group_leader":0}])",
        R"([{"id":1,"group_leader":true,"extra":0}])",
        R"([{"id":1,"group_leader":true},{"id":1,"group_leader":false}])"}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"tasks\":") + Tasks + "}}");
    EXPECT_FALSE(bool(Bad)) << Tasks;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
  LinuxKernelOptions Kernel;
  Kernel.Tasks.emplace();
  for (uint32_t I = 0; I != KernelTaskLimit; ++I)
    Kernel.Tasks->emplace(I + 1, LinuxKernelTask{true});
  ASSERT_FALSE(bool(validateKernelOptions(Kernel)));
  Kernel.Tasks->emplace(KernelTaskLimit + 1, LinuxKernelTask{true});
  auto TooMany = validateKernelOptions(Kernel);
  ASSERT_TRUE(bool(TooMany));
  EXPECT_EQ(llvm::toString(std::move(TooMany)), KernelOptions);

  ProcessOptions Options;
  Options.Android.emplace().ThreadLimit = 3;
  auto &Tasks = Options.LinuxKernel.emplace().Tasks.emplace();
  Tasks.emplace(ThreadID + 1, LinuxKernelTask{true});
  auto Collision = validateKernelTaskInputs(Options);
  ASSERT_TRUE(bool(Collision));
  EXPECT_EQ(llvm::toString(std::move(Collision)), KernelOptions);
  Tasks.clear();
  Options.LinuxPriority.emplace().Tasks.emplace(2000, 0);
  auto Missing = validateKernelTaskInputs(Options);
  ASSERT_TRUE(bool(Missing));
  EXPECT_EQ(llvm::toString(std::move(Missing)), KernelOptions);
  Tasks.emplace(2000, LinuxKernelTask{true});
  EXPECT_FALSE(bool(validateKernelTaskInputs(Options)));
}

TEST_P(LinuxPIDFDProcess, ProcessCPUClocksRetainIdentityAndIdleSeparation) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    auto &Kernel = Options.LinuxKernel.emplace();
    Kernel.GKI = K.Kernel;
    Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
    Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    auto &Time = Options.LinuxTime.emplace();
    Time.AdvanceOnIdle = true;
    Time.Clocks = {{1, {10, 999999999}}, {2, {3, 4}},       {-16006, {7, 9}},
                   {-16007, {8, 10}},    {-16008, {9, 11}}, {-8, {13, 15}},
                   {-8007, {16, 17}}};
    for (const char *Opt : {"O0", "O2"}) {
      auto R = run('p', Opt);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxPIDFDProcess, ProcessCPUClocksAdmitIndependentTaskObservations) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
  Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
  Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
  auto &Time = Options.LinuxTime.emplace();
  Time.AdvanceOnIdle = true;
  Time.Clocks = {{1, {10, 999999999}}, {2, {3, 4}},       {-16006, {7, 9}},
                 {-16007, {8, 10}},    {-16008, {9, 11}}, {-8, {13, 15}},
                 {-8007, {16, 17}}};
  for (const char *Opt : {"O0", "O2"}) {
    auto R = run('p', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
  }
}
TEST_P(LinuxPIDFDProcess, TaskCatalogueDoesNotSelectPIDFDKernelSemantics) {
  Options.LinuxKernel.emplace().Tasks.emplace();
  auto R = run('t', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R.Services.empty());
  EXPECT_EQ(R.Services.back().Number, 434u);
  EXPECT_FALSE(R.Services.back().Result);
}
TEST_P(LinuxPIDFDProcess, ZeroTimeoutPollRetainsReadinessAndOrderedCopies) {
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    auto &Kernel = Options.LinuxKernel.emplace();
    Kernel.GKI = K.Kernel;
    Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
    Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    Options.LinuxTime.emplace().Clocks[1] = {31, 0};
    Options.LinuxTime->AdvanceOnIdle = true;
    for (const char *Opt : {"O0", "O2"}) {
      SCOPED_TRACE(Opt);
      auto R = run('o', Opt, K.ThreadFlag);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << processResultJSON(R);
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxPIDFDProcess, PollUsesGenericFilesButRetainsUnknownBoundaries) {
  auto R = run('h', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, 0u);
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android17_6_18;
  R = run('j', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, PollSignalMaskUnsupported);
  EXPECT_FALSE(R.Services.back().Result);
  R = run('k', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "positive poll timeout requires virtual idle time");
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (char Mode : {'k', 'r'}) {
    R = run(Mode, "O2");
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
  }
  Options.LinuxFiles.reset();
  R = run('h', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, PollLimitMissing);
}

TEST_P(LinuxPIDFDProcess, ProcessCPUClocksKeepMissingObservationBoundaries) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.GKI = AndroidGKIKernel::Android17_6_18;
  Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
  Options.LinuxTime.emplace().Clocks.emplace(2, LinuxTimespec{3, 4});
  auto R = run('q', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(R.Services.back().Result);
  EXPECT_EQ(R.Diagnostic,
            "Linux clock -16006 has no explicit linux_time input");
  Kernel.Tasks.reset();
  R = run('q', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, TimeCPUTargetMissing);
  R = run('z', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, TimeDynamicClock);
  R = run('w', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, TimeThreadCPUClock);
  Options.LinuxKernel.reset();
  R = run('q', "O2");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, TimeDynamicClock);
}
TEST(LinuxPIDFD, ProcessCPUClockValuesRequireLiveObservedGroupsBeforeLoading) {
  for (
      const char *Value :
      {R"({"linux_kernel":{"tasks":[]},"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})",
       R"({"linux_kernel":{"tasks":[{"id":1,"group_leader":false}]},"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})",
       R"({"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})",
       R"({"linux_kernel":{"gki":"android17-6.18"},"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})",
       R"({"linux_kernel":{"gki":"android17-6.18","tasks":[]},"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})",
       R"({"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":1,"group_leader":false}]},"linux_time":{"clocks":[{"id":-14,"seconds":0,"nanoseconds":0}]}})"}) {
    auto O = processOptionsFromJSON(Value);
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), TimeCPUObservation);
  }
  for (
      const char *Clocks :
      {R"([{"id":2,"seconds":3,"nanoseconds":4},{"id":-6,"seconds":3,"nanoseconds":4}])",
       R"([{"id":-8,"seconds":3,"nanoseconds":4},{"id":-8008,"seconds":3,"nanoseconds":4}])",
       R"([{"id":-29,"seconds":0,"nanoseconds":0}])",
       R"([{"id":-2,"seconds":0,"nanoseconds":0}])",
       R"([{"id":-1,"seconds":0,"nanoseconds":0}])",
       R"([{"id":-14,"seconds":-1,"nanoseconds":0}])"}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"linux_time\":{\"clocks\":") + Clocks + "}}");
    EXPECT_FALSE(bool(Bad)) << Clocks;
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, LinuxPIDFDProcess,
                         testing::ValuesIn(Profiles));
} // namespace
} // namespace neverd::emulation

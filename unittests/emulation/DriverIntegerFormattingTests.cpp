//===- DriverIntegerFormattingTests.cpp - Integer string execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify original and genuine WDK formatting in entry, worker and unload.

#include "gtest/gtest.h"
#include <chrono>
#include <iostream>

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelIntegerFormattingExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {
protected:
  void run(const std::filesystem::path &Image) {
    const auto [Backend, Contract, Rebase] = GetParam();
    auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Probe->CPU.reset();
    for (bool Scheduled : {false, true})
      for (char Mode : {'S', 'H'}) {
        SCOPED_TRACE(::testing::Message()
                     << Image << ' ' << Scheduled << ' ' << Mode);
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        // Three complete value/capacity matrices exceed the small default.
        Options.InstructionLimit = 500000;
        // This checks formatting and scheduling, not host throughput. Keep the
        // instruction bound while allowing contended runners to finish.
        Options.TimeoutMilliseconds = 30000;
        Options.ServiceName = std::string("IntegerFormatting") + Mode;
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        std::cerr << "DIAG begin mode=" << Mode << " scheduled=" << Scheduled << std::endl;
        const auto DiagStart = std::chrono::steady_clock::now();
        auto Result = emulateDriver(Image, Options);
        std::cerr << "DIAG end mode=" << Mode << " scheduled=" << Scheduled
                  << " seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-DiagStart).count();
        if (Result) std::cerr << " stop=" << unsigned(Result->Stop) << " instructions=" << Result->Instructions << " diagnostic=" << Result->Diagnostic;
        std::cerr << std::endl;
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        if (Mode == 'H') {
          EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
              << Result->Diagnostic;
          EXPECT_NE(Result->Diagnostic.find("RtlInt64ToUnicodeString"),
                    std::string::npos);
          EXPECT_FALSE(Result->UnloadCompleted);
          continue;
        }
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        EXPECT_EQ(Result->NTStatus, 0u);
        EXPECT_TRUE(Result->Devices.empty());
        EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name == "RtlInt64ToUnicodeString";
                                }),
                  102);
        EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name ==
                                         "RtlIntegerToUnicodeString";
                                }),
                  3);
      }
  }
};

TEST_P(KernelIntegerFormattingExecution,
       OriginalImportsAndLookupPreserveAllValueBits) {
  run(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
      "driver_int64_strings.sys");
}
TEST_P(KernelIntegerFormattingExecution, GenuineWdkCallsPreserveAllValueBits) {
#if !defined(NEVERD_WDM_INT64_STRINGS_FIXTURE) ||                              \
    !defined(NEVERD_WDM_INT64_STRINGS_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK integer fixture was not supplied";
#else
  run(NEVERD_WDM_INT64_STRINGS_FIXTURE);
  if (HasFatalFailure() || IsSkipped())
    return;
#ifdef NEVERD_WDM_INT64_STRINGS_CFG_FIXTURE
  run(NEVERD_WDM_INT64_STRINGS_CFG_FIXTURE);
#endif
#endif
}

std::string integerParameterName(
    const testing::TestParamInfo<KernelIntegerFormattingExecution::ParamType>
        &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelIntegerFormattingExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    integerParameterName);
} // namespace
} // namespace neverd::emulation

//===- AndroidBinderTests.cpp - Binder control and looper state boundaries
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original guest callers across relocation packing and CPU transports.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidBinder : public testing::TestWithParam<
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
    auto &D = Options.LinuxFiles.emplace().Devices["/dev/binder"];
    D.AccessMode = 2;
    D.Binder = true;
    Options.Android->ThreadLimit = 3;
    Options.InstructionQuantum = 83;
    Options.Limits.Instructions = 200000;
    Options.Limits.Events = 5000;
    Options.Limits.TimeoutMicroseconds = 30000000;
#endif
  }
  ProcessResult run(const char *Entry) {
    Options.Android->EntrySymbol = Entry;
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
};
TEST_P(AndroidBinder, ControlProtocolAndExactGuardedOutputs) {
  auto R = run("binder_control");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidBinder, AliasesShareStateAndSeparateOpensDoNot) {
  auto R = run("binder_descriptions");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidBinder, KernelThreadsHaveSeparateLoopers) {
  auto R = run("binder_threads");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidBinder, MissingPeersAndTransactionsDoNotSucceed) {
  for (const char *Entry :
       {"binder_unknown_transaction", "binder_blocking",
        "binder_partial_command", "binder_oneway_transaction"}) {
    SCOPED_TRACE(Entry);
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("Binder"), std::string::npos) << R.Diagnostic;
  }
}
TEST_P(AndroidBinder, GuestServicesExecuteSynchronousTransactionsAndReplies) {
  Options.Android->ThreadLimit =
      5; // Four successive service thread identities.
  auto R = run("binder_service_transactions");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidBinder, MissingTargetErrorsRemainQueuedUntilReadCapacityFits) {
  for (const char *Entry :
       {"binder_missing_targets", "binder_object_transaction"}) {
    SCOPED_TRACE(Entry);
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidBinder, LargeArenasRetainMultipleCopiedRequestsAndReplies) {
  auto R = run("binder_large_arena_transactions");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidBinder,
    testing::Combine(testing::Values("binder-O0-none", "binder-O2-none",
                                     "binder-O0-android", "binder-O2-android",
                                     "binder-O0-relr", "binder-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation

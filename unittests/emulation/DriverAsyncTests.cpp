//===- DriverAsyncTests.cpp - Pending IRP guest execution -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify callback execution, ownership and failure observations end to end.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#define NEVERD_ASYNC_CASE(Name, Code) constexpr uint32_t Name = Code;
#include "fixtures/DriverAsyncCases.def"
#undef NEVERD_ASYNC_CASE

DriverOptions scenario(uint32_t Code) {
  DriverOptions Options;
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Options.Requests.push_back(Create);
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = Code;
  IO.Input = {0, 1, 0x5a, 0xff};
  IO.OutputSize = 8;
  Options.Requests.push_back(IO);
  DriverRequest Cleanup;
  Cleanup.Kind = DriverRequestKind::Cleanup;
  Options.Requests.push_back(Cleanup);
  DriverRequest Close;
  Close.Kind = DriverRequestKind::Close;
  Options.Requests.push_back(Close);
  Options.Unload = true;
  return Options;
}

TEST(DriverAsync, WorkerCompletesPendingIrpAndUnloads) {
  for (auto Code : {DeferredSuccess, RequeueCurrent, DeleteInWorker}) {
    auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                                scenario(Code));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_TRUE(Result->Requests[1].Completed);
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{0x5a, 0x5b, 0, 0xa5}));
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                            [](const auto &Call) {
                              return Call.Name == "IoFreeWorkItem" &&
                                     Call.Phase.starts_with("callback:");
                            }));
  }
}

TEST(DriverAsync, PendingDispatchKeepsFailureCompletionSeparate) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                              scenario(DeferredFailure));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_EQ(Result->Requests[1].IOStatus, 0xc000000du);
  EXPECT_TRUE(Result->Requests[1].Completed);
  EXPECT_TRUE(Result->Requests[1].Output.empty());
  EXPECT_TRUE(Result->UnloadCompleted);
}

TEST(DriverAsync, MarkedAlreadyCompletedIrpStillReturnsPending) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                              scenario(EarlyCompletion));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_TRUE(Result->Requests[1].Completed);
  EXPECT_TRUE(Result->UnloadCompleted);
}

TEST(DriverAsync, RejectsInvalidLifetimesAndPendingContracts) {
  const std::pair<uint32_t, const char *> Cases[] = {
      {DoubleQueue, "queued"},
      {FreeQueued, "queued"},
      {MissingMark, "STATUS_PENDING"},
      {MarkedSuccess, "STATUS_PENDING"},
      {EarlyWrongReturn, "STATUS_PENDING"},
      {NoProducer, "stalled"},
      {WorkerNoCompletion, "stalled"},
      {CompletedAccess, "freed"},
      {OpaqueWorkItem, "opaque"},
      {DoubleFree, "live allocated"}};
  for (auto [Code, Diagnostic] : Cases) {
    SCOPED_TRACE(Code);
    auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                                scenario(Code));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find(Diagnostic), std::string::npos)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->UnloadCompleted);
  }
}

TEST(DriverAsync, CallbackFaultPreservesFirstFaultAndPendingObservation) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                              scenario(CallbackFault));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_TRUE(Result->Phase.starts_with("callback:"));
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverAsync, CallbackSharesTheSessionInstructionBudget) {
  auto Options = scenario(WorkerLoop);
  Options.InstructionLimit = 3000;
  auto Result =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::InstructionLimit);
  EXPECT_EQ(Result->Instructions, 3000u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_TRUE(Result->Phase.starts_with("callback:"));
}
} // namespace
} // namespace neverd::emulation

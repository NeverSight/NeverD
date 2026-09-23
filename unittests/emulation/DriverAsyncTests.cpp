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
#include <vector>

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

DriverOptions concurrentScenario() {
  DriverOptions Options;
  for (uint32_t File : {0u, 1u}) {
    DriverRequest Create;
    Create.Kind = DriverRequestKind::Create;
    Create.File = File;
    Options.Requests.push_back(Create);
  }
  for (uint32_t File : {0u, 1u}) {
    DriverRequest IO;
    IO.Kind = DriverRequestKind::DeviceControl;
    IO.ControlCode = BatchDeferred;
    IO.File = File;
    IO.Input = File ? std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}
                    : std::vector<uint8_t>{0, 1, 0x5a, 0xff};
    IO.OutputSize = 4;
    IO.DeferCallbackDrain = File == 0;
    Options.Requests.push_back(IO);
  }
  for (uint32_t File : {0u, 1u})
    for (DriverRequestKind Kind :
         {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest Request;
      Request.Kind = Kind;
      Request.File = File;
      Options.Requests.push_back(Request);
    }
  Options.Unload = true;
  return Options;
}

TEST(DriverAsync, BatchSubmitsIndependentPendingIRPsBeforeEitherWorkerRuns) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys",
                              concurrentScenario());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 8u);
  EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
  EXPECT_EQ(Result->Requests[3].DispatchStatus, 0x103u);
  EXPECT_EQ(Result->Requests[2].Output,
            (std::vector<uint8_t>{0x5a, 0x5b, 0, 0xa5}));
  EXPECT_EQ(Result->Requests[3].Output,
            (std::vector<uint8_t>{0x4b, 0x78, 0x69, 0x1e}));
  EXPECT_TRUE(Result->UnloadCompleted);
  std::vector<uint64_t> CompletedIRPs;
  size_t SecondQueue = Result->Calls.size(), FirstWorker = Result->Calls.size();
  for (size_t I = 0; I < Result->Calls.size(); ++I) {
    const auto &Call = Result->Calls[I];
    if (Call.Name == "IoQueueWorkItem" && Call.Phase == "request:3")
      SecondQueue = I;
    if (Call.Name == "IoFreeWorkItem" && Call.Phase.starts_with("callback:"))
      FirstWorker = std::min(FirstWorker, I);
    if (Call.Name == "IofCompleteRequest" &&
        Call.Phase.starts_with("callback:") && !Call.Arguments.empty())
      CompletedIRPs.push_back(Call.Arguments[0]);
  }
  EXPECT_LT(SecondQueue, FirstWorker);
  EXPECT_EQ(CompletedIRPs, (std::vector<uint64_t>{Result->Requests[2].IRP,
                                                  Result->Requests[3].IRP}));
}

TEST(DriverAsync, FinalDeferredRequestDrainsAtScenarioEnd) {
  auto Options = concurrentScenario();
  Options.Requests.resize(4);
  Options.Requests[3].DeferCallbackDrain = true;
  Options.Unload = false;
  auto Result =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  for (size_t I : {2u, 3u}) {
    EXPECT_EQ(Result->Requests[I].DispatchStatus, 0x103u);
    EXPECT_TRUE(Result->Requests[I].Completed);
    EXPECT_EQ(Result->Requests[I].IOStatus, 0u);
  }
}

TEST(DriverAsync, BatchedCancellationRetiresOnlyItsOwnIRP) {
  auto Options = concurrentScenario();
  Options.Requests[2].ControlCode = BatchCancelable;
  Options.Requests[2].CancelAfter100ns = 0;
  Options.Requests[3].ControlCode = BatchCancelable;
  auto Result =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 8u);
  EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
  EXPECT_EQ(Result->Requests[2].IOStatus, 0xc0000120u);
  EXPECT_TRUE(Result->Requests[2].CancelRequestedAt100ns);
  EXPECT_EQ(Result->Requests[3].DispatchStatus, 0x103u);
  EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
  EXPECT_FALSE(Result->Requests[3].CancelRequestedAt100ns);
  EXPECT_EQ(Result->Requests[3].Output,
            (std::vector<uint8_t>{0x4b, 0x78, 0x69, 0x1e}));
  EXPECT_TRUE(Result->UnloadCompleted);
}

TEST(DriverAsync, DeferredDrainRequiresPendingAndIndependentFile) {
  auto Synchronous = scenario(DeferredSuccess);
  Synchronous.Requests[1].ControlCode = 0x222000;
  Synchronous.Requests[1].DeferCallbackDrain = true;
  // The ordinary software I/O fixture completes this IOCTL synchronously.
  auto SyncResult =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_io.sys", Synchronous);
  ASSERT_TRUE(bool(SyncResult)) << llvm::toString(SyncResult.takeError());
  EXPECT_EQ(SyncResult->Stop, DriverStopReason::ModelError);
  EXPECT_NE(SyncResult->Diagnostic.find("remains pending"), std::string::npos);

  auto SameFile = concurrentScenario();
  SameFile.Requests[3].File = 0;
  auto SameFileResult =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_async.sys", SameFile);
  ASSERT_TRUE(bool(SameFileResult))
      << llvm::toString(SameFileResult.takeError());
  EXPECT_EQ(SameFileResult->Stop, DriverStopReason::ModelError);
  EXPECT_NE(SameFileResult->Diagnostic.find("prior request to finalize"),
            std::string::npos);
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

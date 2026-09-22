//===- DriverKMDFControlTests.cpp - Genuine KMDF control-device I/O -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute an optional original control-device fixture linked through the
/// genuine WDK KMDF entry library, including its real queue and worker
/// callbacks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

#ifdef NEVERD_KMDF_CONTROL_FIXTURE
constexpr uint32_t Pending = 0x103;
constexpr uint32_t BufferTooSmall = 0xc0000023;
constexpr uint32_t InvalidDeviceRequest = 0xc0000010;
constexpr uint32_t DataError = 0xc000003e;
constexpr uint32_t Cancelled = 0xc0000120;

std::vector<const char *> controlImages() {
  std::vector<const char *> Images{NEVERD_KMDF_CONTROL_FIXTURE};
#ifdef NEVERD_KMDF_CONTROL_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_CONTROL_CFG_FIXTURE);
#endif
  return Images;
}

DriverRequest controlRequest(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  return Request;
}

std::vector<uint8_t> pattern(uint8_t Base, size_t Length) {
  std::vector<uint8_t> Bytes;
  for (size_t Index = 0; Index != Length; ++Index)
    Bytes.push_back(Base + (Index & 0x1f));
  return Bytes;
}

DriverOptions controlOptions(char Mode = 'B') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDKmdfControl") + Mode;
  Options.Unload = true;
  auto Create = controlRequest(DriverRequestKind::Create);
  Create.Device = "\\DosDevices\\NeverDKmdfControl";
  Options.Requests.push_back(std::move(Create));
  auto IO = controlRequest(DriverRequestKind::DeviceControl);
  IO.ControlCode = 0x222000;
  IO.Input = {0, 1, 0x5a, 0xff};
  // The fixture checks both logical lengths despite their aliased allocation.
  IO.OutputSize = 19;
  Options.Requests.push_back(std::move(IO));
  auto Read = controlRequest(DriverRequestKind::Read);
  Read.OutputSize = 40;
  Read.ByteOffset = 0x123456789;
  Options.Requests.push_back(std::move(Read));
  auto Write = controlRequest(DriverRequestKind::Write);
  Write.Input = pattern(0x40, 40);
  Write.ByteOffset = 0x23456789a;
  Options.Requests.push_back(std::move(Write));
  Options.Requests.push_back(controlRequest(DriverRequestKind::Cleanup));
  Options.Requests.push_back(controlRequest(DriverRequestKind::Close));
  return Options;
}

std::vector<std::string> controlMessages(const DriverResult &Result) {
  std::vector<std::string> Messages;
  for (const auto &Message : Result.Messages)
    if (llvm::StringRef(Message).starts_with("KMDF control:"))
      Messages.push_back(Message);
  return Messages;
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

void checkCompletedLifecycle(const DriverResult &Result, size_t RequestCount) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  ASSERT_EQ(Result.Requests.size(), RequestCount);
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_NE(Request.IRP, 0u);
    EXPECT_EQ(Request.Device, "\\Device\\NeverDKmdfControl");
    const bool Queued = Request.Kind == DriverRequestKind::DeviceControl ||
                        Request.Kind == DriverRequestKind::Read ||
                        Request.Kind == DriverRequestKind::Write;
    // FxIoQueue marks every request pending before calling the driver, even
    // when its callback completes synchronously.
    EXPECT_EQ(Request.DispatchStatus, Queued ? Pending : 0u);
    if (!Queued) {
      EXPECT_EQ(Request.IOStatus, 0u);
      EXPECT_EQ(Request.Information, 0u);
      EXPECT_TRUE(Request.Output.empty());
    }
  }
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("KMDF control: failure"), std::string::npos)
        << Message;
  EXPECT_EQ(apiCount(Result, "WdfVersionBind"), 1u);
  EXPECT_EQ(apiCount(Result, "WdfVersionUnbind"), 1u);
  EXPECT_EQ(apiCount(Result, "WdfDeviceCreateSymbolicLink"), 1u);
  EXPECT_EQ(apiCount(Result, "WdfControlFinishInitializing"), 1u);
}

void checkSuccessfulTransfers(const DriverResult &Result, char Mode) {
  ASSERT_EQ(Result.Requests.size(), 6u);
  for (const auto &Request : Result.Requests)
    EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Result.Requests[1].Information, 8u);
  EXPECT_EQ(Result.Requests[1].Output,
            (std::vector<uint8_t>{'K', 'M', 'D', uint8_t(Mode), 0x5a, 0x5b, 0,
                                  0xa5}));
  EXPECT_EQ(Result.Requests[2].Information, 40u);
  EXPECT_EQ(Result.Requests[2].Output, pattern(0x60, 40));
  EXPECT_EQ(Result.Requests[2].ByteOffset, 0x123456789u);
  EXPECT_EQ(Result.Requests[3].Information, 40u);
  EXPECT_TRUE(Result.Requests[3].Output.empty());
  EXPECT_EQ(Result.Requests[3].ByteOffset, 0x23456789au);
}

TEST(DriverKMDFControl, BufferedLifecyclePreservesLogicalBufferLengths) {
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, controlOptions());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  checkSuccessfulTransfers(*Result, 'B');
  EXPECT_EQ(
      controlMessages(*Result),
      (std::vector<std::string>{"KMDF control: ready in mode B\n",
                                "KMDF control: transformed 4 bytes in mode B\n",
                                "KMDF control: read 40 bytes\n",
                                "KMDF control: write 40 bytes\n",
                                "KMDF control: driver unload\n"}));
}

TEST(DriverKMDFControl, DirectReadWriteRetainsBufferedIOCTL) {
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, controlOptions('D'));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  checkSuccessfulTransfers(*Result, 'D');
}

TEST(DriverKMDFControl, RequestCleanupKeepsBuffersAndReferenceKeepsContext) {
  std::vector<const char *> Images{NEVERD_KMDF_CONTROL_FIXTURE};
#ifdef NEVERD_KMDF_CONTROL_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_CONTROL_CFG_FIXTURE);
#endif
  for (const auto *Image : Images) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('C');
    Options.LoadAddress = 0x190000000;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    checkSuccessfulTransfers(*Result, 'C');
    EXPECT_EQ(
        controlMessages(*Result),
        (std::vector<std::string>{
            "KMDF control: ready in mode C\n",
            "KMDF control: transformed 4 bytes in mode C\n",
            "KMDF control: C request cleanup\n",
            "KMDF control: C child destroy\n",
            "KMDF control: C retained context\n",
            "KMDF control: C request destroy\n",
            "KMDF control: read 40 bytes\n", "KMDF control: write 40 bytes\n",
            "KMDF control: driver unload\n"}));
  }
}

TEST(DriverKMDFControl, SmallOutputsCompleteFailureAndAllowUnload) {
  for (uint32_t OutputSize : {0u, 3u, 7u}) {
    SCOPED_TRACE(OutputSize);
    auto Options = controlOptions();
    Options.Requests[1].OutputSize = OutputSize;
    auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[1].IOStatus, BufferTooSmall);
    EXPECT_EQ(Result->Requests[1].Information, 0u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
  }
}

TEST(DriverKMDFControl, EmptyInputDoesNotExposeOutputAllocationCapacity) {
  auto Options = controlOptions();
  Options.Requests[1].Input.clear();
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  ASSERT_EQ(Result->Requests.size(), 6u);
  EXPECT_EQ(Result->Requests[1].IOStatus, BufferTooSmall);
  EXPECT_EQ(Result->Requests[1].Information, 0u);
  EXPECT_TRUE(Result->Requests[1].Output.empty());
  auto Retrieve = std::find_if(
      Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
        return Call.Name == "WdfRequestRetrieveInputBuffer";
      });
  ASSERT_NE(Retrieve, Result->Calls.end());
  EXPECT_EQ(Retrieve->Result, BufferTooSmall);
}

TEST(DriverKMDFControl, UnknownIOCTLReachesFifthCallbackArgument) {
  auto Options = controlOptions();
  Options.Requests[1].ControlCode = 0x222004;
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  ASSERT_EQ(Result->Requests.size(), 6u);
  EXPECT_EQ(Result->Requests[1].IOStatus, InvalidDeviceRequest);
  EXPECT_EQ(Result->Requests[1].Information, 0u);
  EXPECT_TRUE(Result->Requests[1].Output.empty());
  EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
  EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
}

TEST(DriverKMDFControl, FailedWriteRetainsBufferedAndDirectStatus) {
  for (char Mode : {'B', 'D'}) {
    SCOPED_TRACE(Mode);
    auto Options = controlOptions(Mode);
    Options.Requests[3].Input.back() ^= 1;
    auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[3].IOStatus, DataError);
    EXPECT_EQ(Result->Requests[3].Information, 0u);
    EXPECT_TRUE(Result->Requests[3].Output.empty());
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
  }
}

TEST(DriverKMDFControl, DeferredWorkerCompletesBeforeCleanupAndUnload) {
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, controlOptions('W'));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  checkSuccessfulTransfers(*Result, 'W');
  EXPECT_EQ(
      controlMessages(*Result),
      (std::vector<std::string>{"KMDF control: ready in mode W\n",
                                "KMDF control: queued deferred request\n",
                                "KMDF control: deferred worker\n",
                                "KMDF control: transformed 4 bytes in mode W\n",
                                "KMDF control: read 40 bytes\n",
                                "KMDF control: write 40 bytes\n",
                                "KMDF control: driver unload\n"}));
  EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
  EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 1u);
}

TEST(DriverKMDFControl, DeferredRequestRetiresBeforeNextSequentialRequest) {
  auto Options = controlOptions('W');
  auto SecondIO = Options.Requests[1];
  SecondIO.Input = {1, 2};
  SecondIO.OutputSize = 6;
  Options.Requests.insert(Options.Requests.begin() + 2, std::move(SecondIO));
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 7);
  ASSERT_EQ(Result->Requests.size(), 7u);
  for (const auto &Request : Result->Requests)
    EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Result->Requests[1].Information, 8u);
  EXPECT_EQ(Result->Requests[1].Output,
            (std::vector<uint8_t>{'K', 'M', 'D', 'W', 0x5a, 0x5b, 0, 0xa5}));
  EXPECT_EQ(Result->Requests[2].Information, 6u);
  EXPECT_EQ(Result->Requests[2].Output,
            (std::vector<uint8_t>{'K', 'M', 'D', 'W', 0x5b, 0x58}));
  EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 2u);
  EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 2u);
}

TEST(DriverKMDFControl, InvalidQueueConfigUnbindsAndDeletesPartialDevice) {
  auto Options = controlOptions('Q');
  Options.Requests.clear();
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0xc0000004u);
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_TRUE(Result->Devices.empty());
  EXPECT_TRUE(Result->Requests.empty());
  EXPECT_TRUE(controlMessages(*Result).empty());
  EXPECT_EQ(apiCount(*Result, "WdfObjectDelete"), 1u);
  EXPECT_EQ(apiCount(*Result, "WdfVersionUnbind"), 1u);
  EXPECT_EQ(apiCount(*Result, "WdfControlFinishInitializing"), 0u);
  auto QueueCreate = std::find_if(
      Result->Calls.begin(), Result->Calls.end(),
      [](const auto &Call) { return Call.Name == "WdfIoQueueCreate"; });
  ASSERT_NE(QueueCreate, Result->Calls.end());
  EXPECT_EQ(QueueCreate->Result, 0xc0000004u);
}

TEST(DriverKMDFControl, RelocatedActiveCFGExecutesQueueAndWorkerCallbacks) {
#ifdef NEVERD_KMDF_CONTROL_CFG_FIXTURE
  auto Options = controlOptions('W');
  Options.LoadAddress = 0x190000000;
  auto Result = emulateDriver(NEVERD_KMDF_CONTROL_CFG_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  checkCompletedLifecycle(*Result, 6);
  checkSuccessfulTransfers(*Result, 'W');
  EXPECT_EQ(Result->ImageBase, Options.LoadAddress);
  EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
  EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 1u);
#else
  GTEST_SKIP()
      << "A genuine WDK control fixture linked with /guard:cf was not supplied";
#endif
}

TEST(DriverKMDFControl, CancelCallbackRetainsContextUntilItsDestroyEpilogue) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('X');
    Options.LoadAddress = 0x190000000;
    Options.Requests[1].CancelAfter100ns = 10;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[1].IOStatus, Cancelled);
    EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, 10u);
    EXPECT_EQ(Result->Requests[1].Information, 0u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
    EXPECT_EQ(apiCount(*Result, "KeDelayExecutionThread"), 1u);
    EXPECT_EQ(
        controlMessages(*Result),
        (std::vector<std::string>{
            "KMDF control: ready in mode X\n",
            "KMDF control: X marked cancelable\n",
            "KMDF control: X cancel callback\n",
            "KMDF control: X request cleanup\n",
            "KMDF control: X cancel callback retained context\n",
            "KMDF control: X request destroy\n",
            "KMDF control: X destroy resumed\n",
            "KMDF control: read 40 bytes\n", "KMDF control: write 40 bytes\n",
            "KMDF control: driver unload\n"}));
  }
}

TEST(DriverKMDFControl,
     CancellationBeforeDispatchDoesNotDeliverCancelCallback) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('X');
    Options.LoadAddress = 0x190000000;
    Options.Requests[1].CancelAfter100ns = 0;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[1].IOStatus, Cancelled);
    EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, 0u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(apiCount(*Result, "KeDelayExecutionThread"), 0u);
    auto Mark = std::find_if(Result->Calls.begin(), Result->Calls.end(),
                             [](const auto &Call) {
                               return Call.Name == "WdfRequestMarkCancelableEx";
                             });
    ASSERT_NE(Mark, Result->Calls.end());
    EXPECT_EQ(Mark->Result, Cancelled);
    EXPECT_EQ(controlMessages(*Result),
              (std::vector<std::string>{"KMDF control: ready in mode X\n",
                                        "KMDF control: X already cancelled\n",
                                        "KMDF control: X request cleanup\n",
                                        "KMDF control: X request destroy\n",
                                        "KMDF control: read 40 bytes\n",
                                        "KMDF control: write 40 bytes\n",
                                        "KMDF control: driver unload\n"}));
  }
}

TEST(DriverKMDFControl, CancelCallbackWaitsForCooperatingWorkerCompletion) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('H');
    Options.LoadAddress = 0x190000000;
    Options.Requests[1].CancelAfter100ns = 10;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[1].IOStatus, Cancelled);
    EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, 10u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 1u);
    EXPECT_EQ(apiCount(*Result, "KeDelayExecutionThread"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 1u);
    auto Unmark = std::find_if(
        Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
          return Call.Name == "WdfRequestUnmarkCancelable";
        });
    ASSERT_NE(Unmark, Result->Calls.end());
    EXPECT_EQ(Unmark->Result, Cancelled);
    EXPECT_EQ(
        controlMessages(*Result),
        (std::vector<std::string>{
            "KMDF control: ready in mode H\n",
            "KMDF control: H marked cancelable\n",
            "KMDF control: H cancel callback\n",
            "KMDF control: H worker owns completion\n",
            "KMDF control: H request cleanup\n",
            "KMDF control: H cancel callback retained context\n",
            "KMDF control: H request destroy\n",
            "KMDF control: H destroy resumed\n",
            "KMDF control: read 40 bytes\n", "KMDF control: write 40 bytes\n",
            "KMDF control: driver unload\n"}));
  }
}

TEST(DriverKMDFControl,
     UnmarkSeparatesTheCancellationFactFromCallbackDelivery) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    for (uint64_t Delay : {10u, 30u}) {
      SCOPED_TRACE(Delay);
      auto Options = controlOptions('U');
      Options.LoadAddress = 0x190000000;
      Options.Requests[1].CancelAfter100ns = Delay;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 6);
      checkSuccessfulTransfers(*Result, 'U');
      ASSERT_EQ(Result->Requests.size(), 6u);
      if (Delay == 10)
        EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, 10u);
      else
        EXPECT_FALSE(Result->Requests[1].CancelRequestedAt100ns);
      EXPECT_EQ(
          controlMessages(*Result),
          (std::vector<std::string>{
              "KMDF control: ready in mode U\n",
              "KMDF control: U marked cancelable\n",
              "KMDF control: U unmarked cancelable\n",
              Delay == 10 ? "KMDF control: U worker cancelled=1\n"
                          : "KMDF control: U worker cancelled=0\n",
              "KMDF control: transformed 4 bytes in mode U\n",
              "KMDF control: read 40 bytes\n", "KMDF control: write 40 bytes\n",
              "KMDF control: driver unload\n"}));
      auto Unmark = std::find_if(
          Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
            return Call.Name == "WdfRequestUnmarkCancelable";
          });
      ASSERT_NE(Unmark, Result->Calls.end());
      EXPECT_EQ(Unmark->Result, 0u);
    }
  }
}

TEST(DriverKMDFControl, CompletingMarkedRequestWithoutUnmarkIsRejected) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('N');
    Options.LoadAddress = 0x190000000;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("UnmarkCancelable"), std::string::npos);
    EXPECT_FALSE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
    EXPECT_FALSE(Result->Requests[1].CancelRequestedAt100ns);
    ASSERT_FALSE(Result->Calls.empty());
    EXPECT_EQ(Result->Calls.back().Name, "WdfRequestComplete");
    EXPECT_FALSE(Result->Calls.back().Result);
    EXPECT_EQ(
        controlMessages(*Result),
        (std::vector<std::string>{"KMDF control: ready in mode N\n",
                                  "KMDF control: N marked cancelable\n"}));
  }
}

#else
TEST(DriverKMDFControl, RequiresOptionalGenuineWDKControlFixture) {
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE must name the real WDK-linked "
                  "control fixture";
}
#endif

} // namespace
} // namespace neverd::emulation

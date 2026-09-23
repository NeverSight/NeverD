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
constexpr uint32_t AccessViolation = 0xc0000005;

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
  IO.ControlCode = Mode == 'T' ? 0x222003 : 0x222000;
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

void checkCompletedLifecycle(const DriverResult &Result, size_t RequestCount);

TEST(DriverKMDFControl, SequentialQueuePresentsWaitingRequestAfterWorker) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('W');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      Options.Requests[2].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'W', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'W', 0x4b, 0x78, 0x69, 0x1e}));
      std::vector<size_t> Queued;
      std::vector<size_t> Freed;
      for (size_t I = 0; I < Result->Calls.size(); ++I) {
        if (Result->Calls[I].Name == "IoQueueWorkItem")
          Queued.push_back(I);
        if (Result->Calls[I].Name == "IoFreeWorkItem")
          Freed.push_back(I);
      }
      ASSERT_EQ(Queued.size(), 2u);
      ASSERT_EQ(Freed.size(), 2u);
      EXPECT_LT(Queued[0], Freed[0]);
      EXPECT_LT(Freed[0], Queued[1]);
      EXPECT_LT(Queued[1], Freed[1]);
    }
}

TEST(DriverKMDFControl, SequentialQueueCancelsWaitingRequestBeforeWorker) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('W');
    Options.Requests[0].AsynchronousFile = true;
    auto Second = Options.Requests[1];
    Second.Input = {0x11, 0x22, 0x33, 0x44};
    Second.CancelAfter100ns = 0;
    Options.Requests.insert(Options.Requests.begin() + 2, Second);
    Options.Requests[1].DeferCallbackDrain = true;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 7u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[2].IOStatus, Cancelled);
    EXPECT_TRUE(Result->Requests[2].Output.empty());
    EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
  }
}

TEST(DriverKMDFControl,
     StoppedSequentialQueueResumesWaitingRequestAfterWorker) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('S');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'S', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'S', 0x4b, 0x78, 0x69, 0x1e}));
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueStop"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueStart"), 1u);
      EXPECT_GE(apiCount(*Result, "WdfIoQueueGetState"), 4u);
      const auto Messages = controlMessages(*Result);
      EXPECT_TRUE(
          std::any_of(Messages.begin(), Messages.end(), [](const auto &Text) {
            return Text.find("resumed queued request") != std::string::npos;
          }));
      EXPECT_FALSE(
          std::any_of(Messages.begin(), Messages.end(), [](const auto &Text) {
            return Text.find("failure") != std::string::npos;
          }));
    }
}

TEST(DriverKMDFControl, StopCompletionCallbackRunsBeforeQueueRestart) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('V');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      for (const auto &Message : Result->Messages)
        SCOPED_TRACE(Message);
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'V', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'V', 0x4b, 0x78, 0x69, 0x1e}));
      const auto Messages = controlMessages(*Result);
      auto Position = [&](llvm::StringRef Needle) {
        return std::find_if(Messages.begin(), Messages.end(),
                            [&](const auto &Text) {
                              return llvm::StringRef(Text).contains(Needle);
                            });
      };
      const auto Stopped = Position("stop completion callback");
      const auto Resumed = Position("resumed queued request");
      const auto Restarted = Position("queue restarted after worker");
      ASSERT_NE(Stopped, Messages.end());
      ASSERT_NE(Resumed, Messages.end());
      ASSERT_NE(Restarted, Messages.end());
      EXPECT_LT(Stopped, Resumed);
      EXPECT_LT(Resumed, Restarted);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueStop"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueStart"), 1u);
      EXPECT_FALSE(
          std::any_of(Messages.begin(), Messages.end(), [](const auto &Text) {
            return Text.find("failure") != std::string::npos;
          }));
    }
}

TEST(DriverKMDFControl, DrainDeliversQueuedRequestBeforeCompletionCallback) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('E');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      for (const auto &Message : Result->Messages)
        SCOPED_TRACE(Message);
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'E', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'E', 0x4b, 0x78, 0x69, 0x1e}));
      const auto Messages = controlMessages(*Result);
      auto Position = [&](llvm::StringRef Needle) {
        return std::find_if(Messages.begin(), Messages.end(),
                            [&](const auto &Text) {
                              return llvm::StringRef(Text).contains(Needle);
                            });
      };
      const auto Drained = Position("drain completion callback");
      const auto Resumed = Position("resumed queued request");
      const auto Restarted = Position("queue restarted after worker");
      ASSERT_NE(Drained, Messages.end());
      ASSERT_NE(Resumed, Messages.end());
      ASSERT_NE(Restarted, Messages.end());
      EXPECT_LT(Resumed, Drained);
      EXPECT_LT(Drained, Restarted);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueDrain"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueStart"), 1u);
      EXPECT_FALSE(
          std::any_of(Messages.begin(), Messages.end(), [](const auto &Text) {
            return Text.find("failure") != std::string::npos;
          }));
    }
}

TEST(DriverKMDFControl, ParallelQueueDeliversTwoPendingRequestsBeforeWorkers) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('P');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      auto Third = Options.Requests[1];
      Third.Input = {1, 2};
      Third.OutputSize = 6;
      Options.Requests.insert(Options.Requests.begin() + 3, Third);
      Options.Requests[1].DeferCallbackDrain = true;
      Options.Requests[2].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 8);
      ASSERT_EQ(Result->Requests.size(), 8u);
      EXPECT_EQ(Result->Requests[1].Information, 8u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'P', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(Result->Requests[2].Information, 8u);
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'P', 0x4b, 0x78, 0x69, 0x1e}));
      EXPECT_EQ(Result->Requests[3].Information, 6u);
      EXPECT_EQ(Result->Requests[3].Output,
                (std::vector<uint8_t>{'K', 'M', 'D', 'P', 0x5b, 0x58}));
      size_t SecondQueued = Result->Calls.size();
      size_t FirstWorker = Result->Calls.size();
      for (size_t I = 0; I < Result->Calls.size(); ++I) {
        const auto &Call = Result->Calls[I];
        if (Call.Name == "IoQueueWorkItem" && Call.Phase == "request:2")
          SecondQueued = I;
        if (Call.Name == "IoFreeWorkItem" &&
            Call.Phase.starts_with("callback:"))
          FirstWorker = std::min(FirstWorker, I);
      }
      EXPECT_LT(SecondQueued, FirstWorker);
      EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 2u);
      EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 2u);
      const auto Messages = controlMessages(*Result);
      ASSERT_FALSE(Messages.empty());
      EXPECT_EQ(Messages.front(), "KMDF control: ready in mode P\n");
    }
}

TEST(DriverKMDFControl, BoundedParallelQueueWaitsForPresentedCompletion) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('F');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {0x11, 0x22, 0x33, 0x44};
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      auto Third = Options.Requests[1];
      Third.Input = {1, 2};
      Third.OutputSize = 6;
      Options.Requests.insert(Options.Requests.begin() + 3, Third);
      Options.Requests[1].DeferCallbackDrain = true;
      Options.Requests[2].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 8);
      ASSERT_EQ(Result->Requests.size(), 8u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'F', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(
          Result->Requests[2].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'F', 0x4b, 0x78, 0x69, 0x1e}));
      EXPECT_EQ(Result->Requests[3].Output,
                (std::vector<uint8_t>{'K', 'M', 'D', 'F', 0x5b, 0x58}));
      std::vector<size_t> Queued;
      std::vector<size_t> Freed;
      for (size_t I = 0; I < Result->Calls.size(); ++I) {
        if (Result->Calls[I].Name == "IoQueueWorkItem")
          Queued.push_back(I);
        if (Result->Calls[I].Name == "IoFreeWorkItem")
          Freed.push_back(I);
      }
      ASSERT_EQ(Queued.size(), 2u);
      ASSERT_EQ(Freed.size(), 2u);
      EXPECT_LT(Queued[0], Freed[0]);
      EXPECT_LT(Freed[0], Queued[1]);
      EXPECT_LT(Queued[1], Freed[1]);
    }
}

TEST(DriverKMDFControl, BoundedParallelQueueCancelsUndeliveredRequest) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('F');
    Options.Requests[0].AsynchronousFile = true;
    auto Second = Options.Requests[1];
    Second.Input = {0x11, 0x22, 0x33, 0x44};
    Second.CancelAfter100ns = 0;
    Options.Requests.insert(Options.Requests.begin() + 2, Second);
    auto Third = Options.Requests[1];
    Third.Input = {1, 2};
    Third.OutputSize = 6;
    Options.Requests.insert(Options.Requests.begin() + 3, Third);
    Options.Requests[1].DeferCallbackDrain = true;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), 8u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[2].IOStatus, Cancelled);
    EXPECT_TRUE(Result->Requests[2].Output.empty());
    EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[3].Output,
              (std::vector<uint8_t>{'K', 'M', 'D', 'F', 0x5b, 0x58}));
    EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
  }
}

TEST(DriverKMDFControl,
     NondefaultAutomaticQueuesForwardAndRespectPresentationLimits) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'A', 'G'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Mode);
        auto Options = controlOptions(Mode);
        Options.LoadAddress = Address;
        Options.Requests[0].AsynchronousFile = true;
        const unsigned ForwardCount = Mode == 'A' ? 2 : 3;
        for (unsigned I = 1; I < ForwardCount; ++I) {
          auto Next = Options.Requests[1];
          Next.Input = {uint8_t(I), uint8_t(I + 1), uint8_t(I + 2),
                        uint8_t(I + 3)};
          Options.Requests.insert(Options.Requests.begin() + 1 + I,
                                  std::move(Next));
        }
        auto Followup = Options.Requests[1];
        Followup.Input = {1, 2};
        Followup.OutputSize = 6;
        Options.Requests.insert(Options.Requests.begin() + 1 + ForwardCount,
                                std::move(Followup));
        for (unsigned I = 1; I <= ForwardCount; ++I)
          Options.Requests[I].DeferCallbackDrain = true;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        checkCompletedLifecycle(*Result, 6 + ForwardCount);
        ASSERT_EQ(Result->Requests.size(), 6u + ForwardCount);
        for (unsigned I = 1; I <= ForwardCount; ++I) {
          EXPECT_EQ(Result->Requests[I].Information, 8u);
          ASSERT_EQ(Result->Requests[I].Output.size(), 8u);
          EXPECT_EQ(Result->Requests[I].Output[3], uint8_t(Mode));
        }
        EXPECT_EQ(
            Result->Requests[1 + ForwardCount].Output,
            (std::vector<uint8_t>{'K', 'M', 'D', uint8_t(Mode), 0x5b, 0x58}));
        EXPECT_EQ(apiCount(*Result, "WdfRequestForwardToIoQueue"),
                  ForwardCount);
        EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), ForwardCount);
        std::vector<size_t> Queued;
        std::vector<size_t> Freed;
        for (size_t I = 0; I < Result->Calls.size(); ++I) {
          if (Result->Calls[I].Name == "IoQueueWorkItem")
            Queued.push_back(I);
          if (Result->Calls[I].Name == "IoFreeWorkItem")
            Freed.push_back(I);
        }
        ASSERT_EQ(Queued.size(), ForwardCount);
        ASSERT_EQ(Freed.size(), ForwardCount);
        const size_t Limit = Mode == 'A' ? 1 : 2;
        EXPECT_LT(Queued[Limit - 1], Freed[0]);
        EXPECT_LT(Freed[0], Queued[Limit]);
      }
}

TEST(DriverKMDFControl,
     SequentialAutomaticDestinationCancelsBeforeCallbackDelivery) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('A');
    Options.Requests[0].AsynchronousFile = true;
    auto Canceled = Options.Requests[1];
    Canceled.Input = {0x11, 0x22, 0x33, 0x44};
    Canceled.CancelAfter100ns = 0;
    Options.Requests.insert(Options.Requests.begin() + 2, Canceled);
    auto Followup = Options.Requests[1];
    Followup.Input = {1, 2};
    Followup.OutputSize = 6;
    Options.Requests.insert(Options.Requests.begin() + 3, Followup);
    Options.Requests[1].DeferCallbackDrain = true;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 8u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[2].IOStatus, Cancelled);
    EXPECT_TRUE(Result->Requests[2].Output.empty());
    EXPECT_EQ(Result->Requests[3].Output,
              (std::vector<uint8_t>{'K', 'M', 'D', 'A', 0x5b, 0x58}));
    EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 1u);
  }
}

TEST(DriverKMDFControl,
     ManualQueueReleasesSequentialSourceAndRetrievesInWorker) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('Y');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {1, 2};
      Second.OutputSize = 6;
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'Y', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(Result->Requests[2].Output,
                (std::vector<uint8_t>{'K', 'M', 'D', 'Y', 0x5b, 0x58}));
      EXPECT_EQ(apiCount(*Result, "WdfRequestForwardToIoQueue"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueRetrieveNextRequest"), 2u);
      const auto Messages = controlMessages(*Result);
      auto Forward = std::find(Messages.begin(), Messages.end(),
                               "KMDF control: forwarded manual request\n");
      auto Followup =
          std::find(Messages.begin(), Messages.end(),
                    "KMDF control: transformed 2 bytes in mode Y\n");
      auto Worker =
          std::find(Messages.begin(), Messages.end(),
                    "KMDF control: manual worker retrieved request\n");
      EXPECT_LT(std::distance(Messages.begin(), Forward),
                std::distance(Messages.begin(), Followup));
      EXPECT_LT(std::distance(Messages.begin(), Followup),
                std::distance(Messages.begin(), Worker));
    }
}

TEST(DriverKMDFControl, FrameworkCancelsManualQueueRequestBeforeRetrieval) {
  for (const auto *Image : controlImages()) {
    for (uint64_t CancelAt : {0ULL, 10ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CancelAt);
      auto Options = controlOptions('Z');
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {1, 2};
      Second.OutputSize = 6;
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = CancelAt != 0;
      Options.Requests[1].CancelAfter100ns = CancelAt;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(Result->Requests[1].IOStatus, Cancelled);
      EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, CancelAt);
      EXPECT_TRUE(Result->Requests[1].Output.empty());
      EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
      EXPECT_EQ(Result->Requests[2].Output,
                (std::vector<uint8_t>{'K', 'M', 'D', 'Z', 0x5b, 0x58}));
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueRetrieveNextRequest"), 1u);
      const auto Messages = controlMessages(*Result);
      EXPECT_NE(std::find(Messages.begin(), Messages.end(),
                          "KMDF control: queued manual request cancelled\n"),
                Messages.end());
    }
  }
}

TEST(DriverKMDFControl, ManualRequestRequeueReturnsSameRequestToWorker) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('R');
      Options.LoadAddress = Address;
      Options.Requests[0].AsynchronousFile = true;
      auto Second = Options.Requests[1];
      Second.Input = {1, 2};
      Second.OutputSize = 6;
      Options.Requests.insert(Options.Requests.begin() + 2, Second);
      Options.Requests[1].DeferCallbackDrain = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 7);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(
          Result->Requests[1].Output,
          (std::vector<uint8_t>{'K', 'M', 'D', 'R', 0x5a, 0x5b, 0, 0xa5}));
      EXPECT_EQ(Result->Requests[2].Output,
                (std::vector<uint8_t>{'K', 'M', 'D', 'R', 0x5b, 0x58}));
      EXPECT_EQ(apiCount(*Result, "WdfRequestRequeue"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfIoQueueRetrieveNextRequest"), 3u);
      const auto Messages = controlMessages(*Result);
      auto Requeued =
          std::find(Messages.begin(), Messages.end(),
                    "KMDF control: manual request requeued at head\n");
      auto Retrieved =
          std::find(Messages.begin(), Messages.end(),
                    "KMDF control: manual worker retrieved request\n");
      EXPECT_LT(std::distance(Messages.begin(), Requeued),
                std::distance(Messages.begin(), Retrieved));
    }
}

TEST(DriverKMDFControl, ParallelQueueBatchesTwoIndependentFileObjects) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    DriverOptions Options;
    Options.ServiceName = "NeverDKmdfControlP";
    for (uint32_t File : {0u, 1u}) {
      auto Create = controlRequest(DriverRequestKind::Create);
      Create.File = File;
      Create.Device = "\\DosDevices\\NeverDKmdfControl";
      Options.Requests.push_back(std::move(Create));
    }
    for (uint32_t File : {0u, 1u}) {
      auto IO = controlRequest(DriverRequestKind::DeviceControl);
      IO.File = File;
      IO.ControlCode = 0x222000;
      IO.Input = File ? std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}
                      : std::vector<uint8_t>{0, 1, 0x5a, 0xff};
      IO.OutputSize = 8;
      IO.DeferCallbackDrain = true;
      Options.Requests.push_back(std::move(IO));
    }
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_TRUE(Result->Requests[2].Completed);
    EXPECT_TRUE(Result->Requests[3].Completed);
    EXPECT_EQ(Result->Requests[2].Output,
              (std::vector<uint8_t>{'K', 'M', 'D', 'P', 0x5a, 0x5b, 0, 0xa5}));
    EXPECT_EQ(
        Result->Requests[3].Output,
        (std::vector<uint8_t>{'K', 'M', 'D', 'P', 0x4b, 0x78, 0x69, 0x1e}));
    EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 2u);
    EXPECT_EQ(apiCount(*Result, "IoFreeWorkItem"), 2u);
  }
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

TEST(DriverKMDFControl, CallerContextPreprocessesBeforeDefaultQueue) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('I');
      Options.LoadAddress = Address;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 6);
      checkSuccessfulTransfers(*Result, 'I');
      EXPECT_EQ(apiCount(*Result, "WdfDeviceInitSetIoInCallerContextCallback"),
                1u);
      EXPECT_EQ(apiCount(*Result, "WdfDeviceEnqueueRequest"), 3u);
    }
}

TEST(DriverKMDFControl, CallerContextMustEnqueueOrComplete) {
  for (const auto *Image : controlImages()) {
    auto Options = controlOptions('J');
    Options.Requests.resize(2);
    Options.Unload = false;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("without enqueue or completion"),
              std::string::npos);
  }
}

TEST(DriverKMDFControl, CallerContextCanCompleteWithoutQueueDelivery) {
  for (const auto *Image : controlImages()) {
    auto Options = controlOptions('K');
    Options.Requests.resize(2);
    Options.Requests.push_back(controlRequest(DriverRequestKind::Cleanup));
    Options.Requests.push_back(controlRequest(DriverRequestKind::Close));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_EQ(Result->Requests[1].DispatchStatus, Pending);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_TRUE(Result->Requests[1].Completed);
    EXPECT_EQ(apiCount(*Result, "WdfDeviceEnqueueRequest"), 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
  }
}

TEST(DriverKMDFControl, NeitherBuffersUseRequestOwnedLockedSystemAliases) {
  for (const auto *Image : controlImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = controlOptions('T');
      Options.LoadAddress = Address;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 6);
      checkSuccessfulTransfers(*Result, 'T');
      EXPECT_EQ(apiCount(*Result, "WdfDeviceEnqueueRequest"), 3u);
      EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveUnsafeUserInputBuffer"),
                2u);
      EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveUnsafeUserOutputBuffer"),
                2u);
      EXPECT_EQ(apiCount(*Result, "WdfRequestProbeAndLockUserBufferForRead"),
                2u);
      EXPECT_EQ(apiCount(*Result, "WdfRequestProbeAndLockUserBufferForWrite"),
                2u);
      EXPECT_GE(apiCount(*Result, "WdfMemoryGetBuffer"), 4u);
    }
}

TEST(DriverKMDFControl, NeitherProbeRejectsInaccessibleUserPages) {
  for (const auto *Image : controlImages())
    for (bool InputFault : {true, false}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(InputFault);
      auto Options = controlOptions('T');
      Options.Requests.resize(2);
      Options.Requests[1].UserInputAccess =
          InputFault ? DriverUserPageAccess::NoAccess
                     : DriverUserPageAccess::ReadWrite;
      Options.Requests[1].UserOutputAccess =
          InputFault ? DriverUserPageAccess::ReadWrite
                     : DriverUserPageAccess::ReadOnly;
      Options.Requests.push_back(controlRequest(DriverRequestKind::Cleanup));
      Options.Requests.push_back(controlRequest(DriverRequestKind::Close));
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 4u);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, Pending);
      EXPECT_EQ(Result->Requests[1].IOStatus, AccessViolation);
      EXPECT_TRUE(Result->Requests[1].Completed);
      EXPECT_EQ(apiCount(*Result, "WdfDeviceEnqueueRequest"), 0u);
      EXPECT_TRUE(Result->UnloadCompleted);
    }
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
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveInputWdmMdl"), 2u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveOutputWdmMdl"), 2u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestSetInformation"), 1u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestWdmGetIrp"), 1u);
    bool SawShorterCompletionInformation = false;
    for (const auto &Call : Result->Calls) {
      if (Call.Name == "WdfRequestRetrieveInputWdmMdl" ||
          Call.Name == "WdfRequestRetrieveOutputWdmMdl")
        EXPECT_EQ(Call.Result, 0xc00000e5u); // STATUS_INTERNAL_ERROR.
      if (Call.Name == "WdfRequestGetInformation" ||
          Call.Name == "WdfRequestGetIoQueue")
        EXPECT_EQ(Call.Result, 0u);
      if (Call.Name == "WdfRequestCompleteWithInformation" &&
          Call.Arguments.size() == 4 && Call.Arguments[3] == 7)
        SawShorterCompletionInformation = true;
    }
    // The cleanup callback observes 7 from CompleteWithInformation and writes
    // 8 into the saved IRP. That raw value must drive the final transfer.
    EXPECT_TRUE(SawShorterCompletionInformation);
    EXPECT_EQ(
        controlMessages(*Result),
        (std::vector<std::string>{
            "KMDF control: ready in mode C\n",
            "KMDF control: transformed 4 bytes in mode C\n",
            "KMDF control: C cleanup information checked\n",
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

TEST(DriverKMDFControl, BufferedRequestMetadataAndMDLsShareTheOriginalIRP) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('M');
    Options.LoadAddress = 0x190000000;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    checkSuccessfulTransfers(*Result, 'M');
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveInputWdmMdl"), 2u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveOutputWdmMdl"), 2u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestSetInformation"), 6u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestGetInformation"), 9u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestComplete"), 3u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestCompleteWithInformation"), 0u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestGetFileObject"), 3u);
    for (size_t Index : {1u, 2u, 3u}) {
      EXPECT_TRUE(std::any_of(
          Result->Calls.begin(), Result->Calls.end(), [&](const auto &Call) {
            return Call.Name == "WdfRequestWdmGetIrp" &&
                   Call.Result == Result->Requests[Index].IRP;
          }));
    }
    EXPECT_EQ(controlMessages(*Result),
              (std::vector<std::string>{
                  "KMDF control: ready in mode M\n",
                  "KMDF control: transformed 4 bytes in mode M\n",
                  "KMDF control: M metadata checked\n",
                  "KMDF control: read 40 bytes\n",
                  "KMDF control: M metadata checked\n",
                  "KMDF control: write 40 bytes\n",
                  "KMDF control: M metadata checked\n",
                  "KMDF control: driver unload\n"}));
  }
}

TEST(DriverKMDFControl, DirectTransfersExposeOriginalMDLsAndSharedInformation) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    auto Options = controlOptions('D');
    Options.LoadAddress = 0x190000000;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    checkCompletedLifecycle(*Result, 6);
    checkSuccessfulTransfers(*Result, 'D');
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveInputWdmMdl"), 1u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestRetrieveOutputWdmMdl"), 1u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestSetInformation"), 4u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestGetInformation"), 6u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestComplete"), 2u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestCompleteWithInformation"), 1u);
    EXPECT_EQ(apiCount(*Result, "WdfRequestGetFileObject"), 2u);
    for (const auto &Call : Result->Calls)
      if (Call.Name == "WdfRequestGetFileObject")
        EXPECT_EQ(Call.Result, 0u);
  }
}

TEST(DriverKMDFControl,
     LegacyMarkCancellationDistinguishesSynchronousAndFutureDelivery) {
  for (const auto *Image : controlImages()) {
    SCOPED_TRACE(Image);
    for (uint64_t Delay : {0u, 10u}) {
      SCOPED_TRACE(Delay);
      auto Options = controlOptions('L');
      Options.LoadAddress = 0x190000000;
      Options.Requests[1].CancelAfter100ns = Delay;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      checkCompletedLifecycle(*Result, 6);
      ASSERT_EQ(Result->Requests.size(), 6u);
      EXPECT_EQ(Result->Requests[1].IOStatus, Cancelled);
      EXPECT_EQ(Result->Requests[1].CancelRequestedAt100ns, Delay);
      EXPECT_EQ(Result->Requests[1].Information, 0u);
      EXPECT_TRUE(Result->Requests[1].Output.empty());
      EXPECT_EQ(apiCount(*Result, "WdfRequestMarkCancelable"), 1u);
      EXPECT_EQ(apiCount(*Result, "WdfRequestMarkCancelableEx"), 0u);
      EXPECT_EQ(apiCount(*Result, "KeDelayExecutionThread"), 1u);
      std::vector<std::string> Expected{
          "KMDF control: ready in mode L\n",
          "KMDF control: L before mark\n",
          "KMDF control: L cancel callback\n",
          "KMDF control: L request cleanup\n",
          "KMDF control: L cancel callback retained context\n",
          "KMDF control: L request destroy\n",
          "KMDF control: L destroy resumed\n",
          "KMDF control: read 40 bytes\n",
          "KMDF control: write 40 bytes\n",
          "KMDF control: driver unload\n"};
      Expected.insert(Expected.begin() + (Delay == 0 ? 7 : 2),
                      "KMDF control: L after mark\n");
      EXPECT_EQ(controlMessages(*Result), Expected);
    }
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

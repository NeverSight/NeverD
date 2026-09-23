//===- KernelFrameworkRequestTests.cpp - KMDF request lifetime -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test the framework/WDM ownership boundary using a counted request host.
/// Public callback and structure ABI follows Microsoft's KMDF 1.33 headers.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkQueueTestSupport.h"

#include <tuple>

namespace neverd::emulation {
namespace {
using namespace framework_test;

class DriverKernelFrameworkRequest : public DriverKernelFrameworkQueue {
protected:
  static constexpr uint64_t ReadPC = IoControlPC + 0x100;
  static constexpr uint64_t WritePC = IoControlPC + 0x200;
  static constexpr uint64_t DefaultPC = IoControlPC + 0x300;
  static constexpr uint64_t CancelPC = IoControlPC + 0x400;
  static constexpr uint64_t BufferSlot = Driver + 0x2100;
  static constexpr uint64_t LengthSlot = Driver + 0x2108;
  static constexpr uint64_t Parameters = Driver + 0x2200;
  static constexpr uint64_t InputBuffer = Driver + 0x5000;
  static constexpr uint64_t OutputBuffer = Driver + 0x6000;
  static constexpr uint32_t ControlCode = 0x822004;
  struct HostRequest {
    KernelFramework::RequestView View;
    uint64_t Input = InputBuffer, Output = OutputBuffer;
    uint64_t Information = 0;
    uint32_t Status = 0;
    unsigned PendingCalls = 0, CompletionCalls = 0;
    bool Completed = false;
    bool Canceled = false;
  };
  std::map<uint64_t, HostRequest> Packets;
  std::vector<std::pair<uint64_t, bool>> BufferQueries;
  std::vector<std::tuple<uint64_t, uint32_t, uint64_t>> Validations;
  std::vector<std::string> HostEvents;
  uint64_t NextIRP = Driver + 0x3000;
  uint64_t Queue = 0, WdmDevice = 0;
  bool FailValidation = false;
  unsigned CancelQueries = 0;

  llvm::Expected<HostRequest *> livePacket(uint64_t IRP) {
    auto I = Packets.find(IRP);
    if (I == Packets.end() || I->second.Completed)
      return failure("host access after IRP completion or to unknown IRP");
    return &I->second;
  }

  void SetUp() override {
    DriverKernelFrameworkQueue::SetUp();
    put(QueueConfig + 24, ReadPC);
    put(QueueConfig + 32, WritePC);
    WdmDevice = take(invoke("WdfDeviceWdmGetDeviceObject", {Globals, Device}));
    KernelFramework::RequestHost Host;
    Host.View =
        [this](uint64_t IRP) -> llvm::Expected<KernelFramework::RequestView> {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      return (*P)->View;
    };
    Host.Buffer = [this](uint64_t IRP,
                         bool Output) -> llvm::Expected<uint64_t> {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      BufferQueries.emplace_back(IRP, Output);
      return Output ? (*P)->Output : (*P)->Input;
    };
    Host.MarkPending = [this](uint64_t IRP) -> llvm::Error {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      ++(*P)->PendingCalls;
      HostEvents.push_back("pending");
      return llvm::Error::success();
    };
    Host.IsCanceled = [this](uint64_t IRP) -> llvm::Expected<bool> {
      ++CancelQueries;
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      return (*P)->Canceled;
    };
    Host.Information = [this](uint64_t IRP) -> llvm::Expected<uint64_t> {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      return (*P)->Information;
    };
    Host.SetInformation = [this](uint64_t IRP,
                                 uint64_t Information) -> llvm::Error {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      (*P)->Information = Information;
      return llvm::Error::success();
    };
    Host.ValidateCompletion = [this](uint64_t IRP, uint32_t Status,
                                     uint64_t Information) -> llvm::Error {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      Validations.emplace_back(IRP, Status, Information);
      if (Status == 0x103)
        return failure("cannot complete an IRP with STATUS_PENDING");
      if (FailValidation)
        return failure("injected validation failure");
      return llvm::Error::success();
    };
    Host.Complete = [this](uint64_t IRP, uint32_t Status,
                           uint64_t Information) -> llvm::Error {
      auto P = livePacket(IRP);
      if (!P)
        return P.takeError();
      auto &Packet = **P;
      ++Packet.CompletionCalls;
      Packet.Completed = true;
      Packet.Status = Status;
      Packet.Information = Information;
      HostEvents.push_back("complete");
      return llvm::Error::success();
    };
    Model.setRequestHost(std::move(Host));
  }

  void initializeQueue() {
    EXPECT_EQ(take(createQueue()), 0u);
    Queue = get(QueueSlot);
    take(invoke("WdfControlFinishInitializing", {Globals, Device}));
  }

  uint64_t manualQueue() {
    queueConfiguration();
    put(QueueConfig + 4, framework::QueueDispatchManual, 4);
    put(QueueConfig + 13, 0, 1);
    put(QueueConfig + 40, 0);
    EXPECT_EQ(take(createQueue()), 0u);
    return get(QueueSlot);
  }

  uint64_t
  automaticQueue(uint32_t Dispatch = framework::QueueDispatchSequential,
                 uint32_t Limit = 0) {
    queueConfiguration();
    put(QueueConfig + 13, 0, 1);
    put(QueueConfig + 4, Dispatch, 4);
    put(QueueConfig + 40, DefaultPC);
    if (Dispatch == framework::QueueDispatchParallel)
      put(QueueConfig + 80, Limit, 4);
    EXPECT_EQ(take(createQueue()), 0u);
    return get(QueueSlot);
  }

  uint64_t defaultAndAutomaticQueue(
      uint32_t Dispatch = framework::QueueDispatchSequential,
      uint32_t Limit = 0) {
    EXPECT_EQ(take(createQueue()), 0u);
    Queue = get(QueueSlot);
    const auto Automatic = automaticQueue(Dispatch, Limit);
    take(invoke("WdfControlFinishInitializing", {Globals, Device}));
    return Automatic;
  }

  uint64_t packet(uint32_t Major = 14, uint32_t Input = 3, uint32_t Output = 7,
                  uint64_t Offset = 0) {
    const uint64_t IRP = NextIRP;
    NextIRP += 0x100;
    HostRequest P;
    P.View = {IRP, Offset, Major, Major == 14 ? ControlCode : 0, Input, Output};
    Packets.emplace(IRP, P);
    return IRP;
  }

  KernelFramework::RequestDispatch route(uint64_t IRP) {
    auto Result = take(Model.routeRequest(WdmDevice, IRP));
    EXPECT_TRUE(Result.has_value());
    return Result.value_or(KernelFramework::RequestDispatch{});
  }

  uint64_t request(const KernelFramework::RequestDispatch &Dispatch) {
    EXPECT_GE(Dispatch.Arguments.size(), 2u);
    return Dispatch.Arguments.size() >= 2 ? Dispatch.Arguments[1] : 0;
  }

  void complete(uint64_t Request, uint64_t Information = 0,
                uint32_t Status = 0) {
    take(invoke("WdfRequestCompleteWithInformation",
                {Globals, Request, Status, Information}));
  }

  uint64_t retrieve(uint64_t Request, bool Output, uint64_t Minimum = 0,
                    bool WithLength = true) {
    put(BufferSlot, Sentinel);
    put(LengthSlot, Sentinel);
    return take(invoke(
        Output ? "WdfRequestRetrieveOutputBuffer"
               : "WdfRequestRetrieveInputBuffer",
        {Globals, Request, Minimum, BufferSlot, WithLength ? LengthSlot : 0}));
  }

  void markCancelable(uint64_t Request) {
    EXPECT_EQ(take(invoke("WdfRequestMarkCancelableEx",
                          {Globals, Request, CancelPC})),
              0u);
  }

  KernelFramework::GuestCall cancel(uint64_t IRP) {
    Packets.at(IRP).Canceled = true;
    auto Call = take(Model.requestCancellation(IRP));
    EXPECT_TRUE(Call);
    EXPECT_FALSE(Model.takeGuestCall());
    return Call.value_or(KernelFramework::GuestCall{});
  }

  uint64_t requestContext(uint64_t Request) {
    type();
    attributes(0, Type, ChildCleanup, ChildDestroy);
    take(invoke("WdfObjectAllocateContext",
                {Globals, Request, Attrs, ContextOutput}));
    return get(ContextOutput);
  }
};

TEST_F(DriverKernelFrameworkRequest,
       IoctlHasFiveArgumentsAndOwnedPendingStatus) {
  initializeQueue();
  const auto IRP = packet();
  auto Dispatch = route(IRP);
  const auto Request = request(Dispatch);
  EXPECT_EQ(Dispatch.PC, IoControlPC);
  EXPECT_EQ(Dispatch.Arguments,
            (std::vector<uint64_t>{Queue, Request, 7, 3, ControlCode}));
  EXPECT_EQ(Dispatch.Status, 0x103u);
  EXPECT_EQ(Packets.at(IRP).PendingCalls, 1u);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_FALSE(Model.takeGuestCall());
  complete(Request, 5);
  EXPECT_EQ(HostEvents, (std::vector<std::string>{"pending", "complete"}));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_EQ(Packets.at(IRP).Information, 5u);
  // A void I/O callback has no completion-status result to substitute here.
  EXPECT_EQ(Dispatch.Status, 0x103u);
}

TEST_F(DriverKernelFrameworkRequest,
       ReadWriteAndFallbackUseTheirOwnCallbackABI) {
  put(QueueConfig + 24, 0);
  put(QueueConfig + 16, DefaultPC);
  initializeQueue();
  const auto ReadIRP = packet(3, 0, 9);
  auto Dispatch = route(ReadIRP);
  EXPECT_EQ(Dispatch.PC, DefaultPC);
  EXPECT_EQ(Dispatch.Arguments,
            (std::vector<uint64_t>{Queue, request(Dispatch)}));
  complete(request(Dispatch));
  const auto WriteIRP = packet(4, 6, 0);
  Dispatch = route(WriteIRP);
  EXPECT_EQ(Dispatch.PC, WritePC);
  EXPECT_EQ(Dispatch.Arguments,
            (std::vector<uint64_t>{Queue, request(Dispatch), 6}));
  complete(request(Dispatch));
}

TEST_F(DriverKernelFrameworkRequest,
       FilePackageCompletesWithoutQueueOrPending) {
  take(invoke("WdfControlFinishInitializing", {Globals, Device}));
  for (uint32_t Major : {0, 18, 2}) {
    const auto IRP = packet(Major, 0, 0);
    const auto Dispatch = route(IRP);
    EXPECT_EQ(Dispatch.PC, 0u);
    EXPECT_TRUE(Dispatch.Arguments.empty());
    EXPECT_EQ(Dispatch.Status, 0u);
    EXPECT_EQ(Packets.at(IRP).PendingCalls, 0u);
    EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
    EXPECT_EQ(Packets.at(IRP).Information, 0u);
  }
}

TEST_F(DriverKernelFrameworkRequest,
       DeviceIdentityAndInitializationGateRouting) {
  const auto IRP = packet();
  EXPECT_FALSE(take(Model.routeRequest(WdmDevice + 1, IRP)));
  expectError(Model.routeRequest(WdmDevice, IRP), "fully initialized");
  EXPECT_TRUE(HostEvents.empty());
  take(invoke("WdfControlFinishInitializing", {Globals, Device}));
  const auto Dispatch = route(IRP);
  EXPECT_EQ(Dispatch.PC, 0u);
  EXPECT_EQ(Dispatch.Status, 0xc0000010u);
  EXPECT_EQ(Packets.at(IRP).Status, 0xc0000010u);
  EXPECT_EQ(Packets.at(IRP).PendingCalls, 0u);
}

TEST_F(DriverKernelFrameworkRequest, MissingHandlerCompletesAnAcceptedRequest) {
  put(QueueConfig + 24, 0);
  initializeQueue();
  const auto IRP = packet(3, 0, 4);
  const auto Attempts = AllocationAttempts;
  const auto Dispatch = route(IRP);
  EXPECT_EQ(Dispatch.PC, 0u);
  EXPECT_TRUE(Dispatch.Arguments.empty());
  EXPECT_EQ(Dispatch.Status, 0x103u);
  EXPECT_EQ(Packets.at(IRP).Status, 0xc0000010u);
  EXPECT_EQ(Packets.at(IRP).Information, 0u);
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(HostEvents, (std::vector<std::string>{"pending", "complete"}));
}

TEST_F(DriverKernelFrameworkRequest,
       ZeroLengthReadWriteCompleteWithoutDelivery) {
  initializeQueue();
  for (uint32_t Major : {3, 4}) {
    const auto IRP = packet(Major, 0, 0);
    const auto Attempts = AllocationAttempts;
    const auto Dispatch = route(IRP);
    EXPECT_EQ(Dispatch.PC, 0u);
    EXPECT_TRUE(Dispatch.Arguments.empty());
    EXPECT_EQ(Dispatch.Status, 0x103u);
    EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
    EXPECT_EQ(Packets.at(IRP).Status, 0u);
    EXPECT_EQ(AllocationAttempts, Attempts);
  }
}

TEST_F(DriverKernelFrameworkRequest, ExplicitZeroLengthDeliveryRetainsReadABI) {
  put(QueueConfig + 12, 1, 1);
  initializeQueue();
  const auto IRP = packet(3, 0, 0);
  const auto Dispatch = route(IRP);
  const auto Request = request(Dispatch);
  EXPECT_EQ(Dispatch.PC, ReadPC);
  EXPECT_EQ(Dispatch.Arguments, (std::vector<uint64_t>{Queue, Request, 0}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(retrieve(Request, true), 0xc0000023u);
  EXPECT_EQ(get(BufferSlot), 0u);
  EXPECT_EQ(get(LengthSlot), 0u);
  complete(Request);
}

TEST_F(DriverKernelFrameworkRequest,
       BufferLengthsAreLogicalEvenWhenStorageAliases) {
  initializeQueue();
  const auto IRP = packet();
  Packets.at(IRP).Output = InputBuffer;
  const auto Request = request(route(IRP));
  EXPECT_EQ(retrieve(Request, false, 4), 0xc0000023u);
  EXPECT_EQ(get(BufferSlot), 0u);
  EXPECT_EQ(get(LengthSlot), 0u);
  EXPECT_TRUE(BufferQueries.empty());
  EXPECT_EQ(retrieve(Request, false, 3), 0u);
  EXPECT_EQ(get(BufferSlot), InputBuffer);
  EXPECT_EQ(get(LengthSlot), 3u);
  EXPECT_EQ(retrieve(Request, true, 7), 0u);
  EXPECT_EQ(get(BufferSlot), InputBuffer);
  EXPECT_EQ(get(LengthSlot), 7u);
  EXPECT_EQ(retrieve(Request, true, 8), 0xc0000023u);
  EXPECT_EQ(get(BufferSlot), 0u);
  EXPECT_EQ(get(LengthSlot), 0u);
  EXPECT_EQ(BufferQueries, (std::vector<std::pair<uint64_t, bool>>{
                               {IRP, false}, {IRP, true}}));
  EXPECT_EQ(retrieve(Request, false, 0, false), 0u);
  EXPECT_EQ(get(BufferSlot), InputBuffer);
  EXPECT_EQ(get(LengthSlot), Sentinel);
  complete(Request);
}

TEST_F(DriverKernelFrameworkRequest, WrongBufferDirectionClearsBothOutputs) {
  initializeQueue();
  for (uint32_t Major : {3, 4}) {
    const auto IRP = packet(Major, Major == 4 ? 3 : 0, Major == 3 ? 7 : 0);
    const auto Request = request(route(IRP));
    EXPECT_EQ(retrieve(Request, Major == 4), 0xc0000010u);
    EXPECT_EQ(get(BufferSlot), 0u);
    EXPECT_EQ(get(LengthSlot), 0u);
    EXPECT_TRUE(BufferQueries.empty());
    complete(Request);
  }
}

TEST_F(DriverKernelFrameworkRequest,
       MappingFailureAndBadOutputsNeverPublishBuffer) {
  initializeQueue();
  const auto IRP = packet();
  Packets.at(IRP).Input = 0;
  const auto Request = request(route(IRP));
  EXPECT_EQ(retrieve(Request, false), 0xc000009au);
  EXPECT_EQ(get(BufferSlot), 0u);
  EXPECT_EQ(get(LengthSlot), 0u);
  BufferQueries.clear();
  put(BufferSlot, Sentinel);
  put(LengthSlot, Sentinel);
  DenyWriteAt = LengthSlot;
  expectError(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "write validation");
  EXPECT_EQ(get(BufferSlot), Sentinel);
  EXPECT_EQ(get(LengthSlot), Sentinel);
  EXPECT_TRUE(BufferQueries.empty());
  DenyWriteAt = 0;
  complete(Request);
}

TEST_F(DriverKernelFrameworkRequest, RequestParametersUseThePublicX64Layout) {
  initializeQueue();
  for (uint32_t Major : {3, 4, 14}) {
    const uint64_t Offset = 0xfedcba9876543210;
    const auto IRP =
        packet(Major, Major == 3 ? 0 : 3, Major == 4 ? 0 : 7, Offset);
    const auto Request = request(route(IRP));
    success(Memory.write(Parameters, std::vector<uint8_t>(48, 0xa5)));
    put(Parameters, 40, 2);
    take(invoke("WdfRequestGetParameters", {Globals, Request, Parameters}));
    EXPECT_EQ(get(Parameters, 2), 40u);
    EXPECT_EQ(get(Parameters + 2, 1), 0u);
    EXPECT_EQ(get(Parameters + 4, 4), Major);
    EXPECT_EQ(get(Parameters + 8), Major == 4 ? 3u : 7u);
    EXPECT_EQ(get(Parameters + 16), Major == 14 ? 3u : 0u);
    EXPECT_EQ(get(Parameters + 24), Major == 14 ? ControlCode : Offset);
    EXPECT_EQ(get(Parameters + 32), 0u);
    EXPECT_EQ(get(Parameters + 40), 0xa5a5a5a5a5a5a5a5);
    complete(Request);
  }
}

TEST_F(DriverKernelFrameworkRequest, ParameterValidationPreservesCallerMemory) {
  initializeQueue();
  const auto Request = request(route(packet()));
  success(Memory.write(Parameters, std::vector<uint8_t>(40, 0xa5)));
  put(Parameters, 39, 2);
  expectError(invoke("WdfRequestGetParameters", {Globals, Request, Parameters}),
              "size");
  EXPECT_EQ(get(Parameters, 2), 39u);
  EXPECT_EQ(get(Parameters + 8), 0xa5a5a5a5a5a5a5a5);
  put(Parameters, 40, 2);
  DenyWriteAt = Parameters + 39;
  expectError(invoke("WdfRequestGetParameters", {Globals, Request, Parameters}),
              "write validation");
  EXPECT_EQ(get(Parameters + 8), 0xa5a5a5a5a5a5a5a5);
  DenyWriteAt = 0;
  complete(Request);
}

TEST_F(DriverKernelFrameworkRequest,
       CompletionRetiresTheIRPAndRejectsRepeatedUse) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  Packets.at(IRP).Information = 2;
  take(invoke("WdfRequestComplete", {Globals, Request, 0xc0000001}));
  EXPECT_EQ(Packets.at(IRP).Status, 0xc0000001u);
  EXPECT_EQ(Packets.at(IRP).Information, 2u);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_TRUE(Released.count(Request));
  expectError(
      invoke("WdfRequestCompleteWithInformation", {Globals, Request, 0, 0}),
      "completed framework request");
  expectError(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "completed framework request");
  expectError(invoke("WdfRequestGetParameters", {Globals, Request, Parameters}),
              "completed framework request");
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialQueueAcceptsAndPresentsSecondRequestAfterCompletion) {
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  auto Waiting = route(SecondIRP);
  EXPECT_EQ(Waiting.PC, 0u);
  EXPECT_EQ(Waiting.Status, 0x103u);
  EXPECT_EQ(Packets.at(SecondIRP).PendingCalls, 1u);
  complete(First);
  auto Delivered = callback();
  EXPECT_EQ(Delivered.PC, IoControlPC);
  EXPECT_EQ(Delivered.Arguments[0], Queue);
  const auto Second = Delivered.Arguments[1];
  EXPECT_NE(Second, First);
  complete(Second);
  finish(Delivered);
  EXPECT_TRUE(Packets.at(SecondIRP).Completed);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialQueueCancelsUndeliveredRequestWithoutInvokingHandler) {
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(route(SecondIRP).PC, 0u);
  Packets.at(SecondIRP).Canceled = true;
  EXPECT_TRUE(take(Model.preflightRequestCancellation(SecondIRP)));
  EXPECT_FALSE(take(Model.requestCancellation(SecondIRP)));
  EXPECT_EQ(Packets.at(SecondIRP).Status, framework::RequestCancelled);
  complete(First);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialQueueSkipsCanceledWaiterInFifoOrder) {
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto ThirdIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(route(SecondIRP).PC, 0u);
  EXPECT_EQ(route(ThirdIRP).PC, 0u);
  Packets.at(SecondIRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(SecondIRP)));
  complete(First);
  auto Third = callback();
  EXPECT_EQ(Third.PC, IoControlPC);
  EXPECT_EQ(Third.Arguments[0], Queue);
  complete(Third.Arguments[1]);
  finish(Third);
  EXPECT_TRUE(Packets.at(ThirdIRP).Completed);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialQueueDefersAutomaticNoHandlerAndZeroLengthCompletions) {
  put(QueueConfig + 24, 0);
  initializeQueue();
  const auto First = request(route(packet()));
  const auto MissingHandlerIRP = packet(3, 0, 9);
  const auto ZeroLengthIRP = packet(4, 0, 0);
  EXPECT_EQ(route(MissingHandlerIRP).PC, 0u);
  EXPECT_EQ(route(ZeroLengthIRP).PC, 0u);
  EXPECT_FALSE(Packets.at(MissingHandlerIRP).Completed);
  EXPECT_FALSE(Packets.at(ZeroLengthIRP).Completed);
  complete(First);
  EXPECT_TRUE(Packets.at(MissingHandlerIRP).Completed);
  EXPECT_EQ(Packets.at(MissingHandlerIRP).Status,
            framework::ControlInvalidDeviceRequest);
  EXPECT_TRUE(Packets.at(ZeroLengthIRP).Completed);
  EXPECT_EQ(Packets.at(ZeroLengthIRP).Status, 0u);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       ManualDefaultQueueAcceptsRetrievesAndCancelsPendingRequests) {
  put(QueueConfig + 4, framework::QueueDispatchManual, 4);
  put(QueueConfig + 24, 0);
  put(QueueConfig + 32, 0);
  put(QueueConfig + 40, 0);
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto ThirdIRP = packet();
  EXPECT_EQ(route(FirstIRP).PC, 0u);
  EXPECT_EQ(route(SecondIRP).PC, 0u);
  EXPECT_EQ(route(ThirdIRP).PC, 0u);
  EXPECT_EQ(Packets.at(FirstIRP).PendingCalls, 1u);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(ThirdIRP)));
  Packets.at(ThirdIRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(ThirdIRP)));
  EXPECT_EQ(Packets.at(ThirdIRP).Status, framework::RequestCancelled);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Queue, BufferSlot})),
            0u);
  const auto First = get(BufferSlot);
  EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, First})), Queue);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Queue, BufferSlot})),
            0u);
  const auto Second = get(BufferSlot);
  EXPECT_NE(First, Second);
  complete(First, 1);
  complete(Second, 2);
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Queue, BufferSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(BufferSlot), 0u);
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).Information, 2u);
  const auto UnknownIRP = packet(99);
  expectError(Model.routeRequest(WdmDevice, UnknownIRP),
              "unsupported framework request major");
  EXPECT_EQ(Packets.at(UnknownIRP).PendingCalls, 0u);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       ForwardedRequestsLeaveSequentialSourceAndReturnInManualFifoOrder) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Manual})), 0u);
  expectError(invoke("WdfRequestComplete", {Globals, First, 0}),
              "framework owns");
  expectError(invoke("WdfRequestGetIoQueue", {Globals, First}),
              "framework owns");
  expectError(invoke("WdfObjectDelete", {Globals, Manual}), "live request");
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Manual})),
      0u);
  put(BufferSlot, Sentinel);
  DenyWriteAt = BufferSlot;
  expectError(
      invoke("WdfIoQueueRetrieveNextRequest", {Globals, Manual, BufferSlot}),
      "write validation");
  EXPECT_EQ(get(BufferSlot), Sentinel);
  DenyWriteAt = 0;
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), First);
  EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, First})), Manual);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), Second);
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(BufferSlot), 0u);
  complete(Second, 2);
  complete(First, 1);
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).Information, 2u);
  take(invoke("WdfObjectDelete", {Globals, Manual}));
  EXPECT_TRUE(Released.count(Manual));
}

TEST_F(DriverKernelFrameworkRequest,
       ForwardToAutomaticQueueDeliversItsOwnCallbackAndReleasesSource) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto FirstIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  auto Delivered = callback();
  EXPECT_EQ(Delivered.PC, DefaultPC);
  EXPECT_EQ(Delivered.Arguments,
            (std::vector<uint64_t>{Automatic, First, 7, 3, ControlCode}));
  EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, First})), Automatic);
  complete(First, 3);
  finish(Delivered);
  EXPECT_TRUE(Packets.at(FirstIRP).Completed);
  const auto SecondIRP = packet();
  const auto Second = request(route(SecondIRP));
  EXPECT_NE(Second, First);
  complete(Second);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialAutomaticDestinationWaitsAndPresentsAfterCompletion) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  auto FirstCall = callback();
  EXPECT_EQ(FirstCall.PC, DefaultPC);
  finish(FirstCall);
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Automatic})),
      0u);
  EXPECT_FALSE(Model.takeGuestCall());
  expectError(invoke("WdfRequestComplete", {Globals, Second, 0}),
              "framework owns");
  EXPECT_TRUE(take(Model.preflightRequestCancellation(SecondIRP)));
  complete(First, 1);
  auto SecondCall = callback();
  EXPECT_EQ(SecondCall.PC, DefaultPC);
  EXPECT_EQ(SecondCall.Arguments[0], Automatic);
  EXPECT_EQ(SecondCall.Arguments[1], Second);
  complete(Second, 2);
  finish(SecondCall);
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).Information, 2u);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       AutomaticDestinationWithoutMatchingHandlerCompletesInvalidRequest) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto IRP = packet(3, 0, 9);
  const auto Request = request(route(IRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Automatic})),
      0u);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::ControlInvalidDeviceRequest);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       AutomaticDestinationWaitsBeforeRejectingUnmatchedRequest) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto FirstIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  finish(callback());
  const auto SecondIRP = packet(3, 0, 9);
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Automatic})),
      0u);
  EXPECT_FALSE(Packets.at(SecondIRP).Completed);
  complete(First);
  EXPECT_TRUE(Packets.at(SecondIRP).Completed);
  EXPECT_EQ(Packets.at(SecondIRP).Status,
            framework::ControlInvalidDeviceRequest);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       UnmatchedAutomaticDeliveryCleansRequestBeforeHostCompletion) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto IRP = packet(3, 0, 9);
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Automatic})),
      0u);
  auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(get(Context), Sentinel);
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::ControlInvalidDeviceRequest);
  auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       CancelledAutomaticWaiterDoesNotReachItsCallback) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  finish(callback());
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Automatic})),
      0u);
  Packets.at(SecondIRP).Canceled = true;
  EXPECT_TRUE(take(Model.preflightRequestCancellation(SecondIRP)));
  EXPECT_FALSE(take(Model.requestCancellation(SecondIRP)));
  EXPECT_EQ(Packets.at(SecondIRP).Status, framework::RequestCancelled);
  complete(First);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       SequentialAutomaticWaiterMayBeExplicitlyRetrieved) {
  const auto Automatic = defaultAndAutomaticQueue();
  const auto First = request(route(packet()));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  finish(callback());
  const auto Second = request(route(packet()));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Automatic})),
      0u);
  const auto Third = request(route(packet()));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Third, Automatic})),
      0u);
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Automatic, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), Second);
  EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, Second})), Automatic);
  EXPECT_FALSE(Model.takeGuestCall());
  complete(First);
  EXPECT_FALSE(Model.takeGuestCall());
  complete(Second);
  auto Delivered = callback();
  EXPECT_EQ(Delivered.PC, DefaultPC);
  EXPECT_EQ(Delivered.Arguments[1], Third);
  complete(Third);
  finish(Delivered);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       BoundedAutomaticDestinationPresentsOneWaiterPerAvailableSlot) {
  const auto Automatic =
      defaultAndAutomaticQueue(framework::QueueDispatchParallel, 2);
  std::vector<uint64_t> Requests;
  for (unsigned I = 0; I < 3; ++I) {
    const auto Request = request(route(packet()));
    Requests.push_back(Request);
    EXPECT_EQ(take(invoke("WdfRequestForwardToIoQueue",
                          {Globals, Request, Automatic})),
              0u);
    if (I < 2) {
      auto Delivered = callback();
      EXPECT_EQ(Delivered.PC, DefaultPC);
      EXPECT_EQ(Delivered.Arguments[1], Request);
      finish(Delivered);
    }
  }
  EXPECT_FALSE(Model.takeGuestCall());
  complete(Requests[0]);
  auto Third = callback();
  EXPECT_EQ(Third.PC, DefaultPC);
  EXPECT_EQ(Third.Arguments[1], Requests[2]);
  complete(Requests[2]);
  finish(Third);
  complete(Requests[1]);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       ForwardingFreesAWaiterOnTheBoundedSourceQueue) {
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, 1, 4);
  EXPECT_EQ(take(createQueue()), 0u);
  Queue = get(QueueSlot);
  const auto Automatic = automaticQueue();
  take(invoke("WdfControlFinishInitializing", {Globals, Device}));
  const auto First = request(route(packet()));
  EXPECT_EQ(route(packet()).PC, 0u);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Automatic})),
      0u);
  auto DestinationCall = callback();
  EXPECT_EQ(DestinationCall.PC, DefaultPC);
  finish(DestinationCall);
  auto SourceCall = callback();
  EXPECT_EQ(SourceCall.PC, IoControlPC);
  EXPECT_EQ(SourceCall.Arguments[0], Queue);
  const auto Second = SourceCall.Arguments[1];
  complete(Second);
  finish(SourceCall);
  complete(First);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       ForwardingRejectsInvalidOwnershipWithoutMovingTheRequest) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Queue})),
      uint64_t(framework::QueueInvalidDeviceRequest));
  const auto OtherDevice = control();
  Device = OtherDevice;
  const auto ForeignManual = manualQueue();
  Device = take(invoke("WdfIoQueueGetDevice", {Globals, Queue}));
  EXPECT_EQ(take(invoke("WdfRequestForwardToIoQueue",
                        {Globals, Request, ForeignManual})),
            uint64_t(framework::QueueInvalidDeviceRequest));
  markCancelable(Request);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      uint64_t(framework::QueueInvalidDeviceRequest));
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})), 0u);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Queue})),
      uint64_t(framework::QueueInvalidDeviceRequest));
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), Request);
  complete(Request);
}

TEST_F(DriverKernelFrameworkRequest,
       ManualRequestRequeueReturnsToHeadAndTransfersOwnership) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(take(invoke("WdfRequestRequeue", {Globals, First})),
            uint64_t(framework::QueueInvalidDeviceRequest));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, First, Manual})), 0u);
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Second, Manual})),
      0u);
  EXPECT_EQ(take(invoke("WdfRequestRequeue", {Globals, First})),
            uint64_t(framework::QueueInvalidDeviceRequest));
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), First);
  markCancelable(First);
  EXPECT_EQ(take(invoke("WdfRequestRequeue", {Globals, First})),
            uint64_t(framework::QueueInvalidDeviceRequest));
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, First})), 0u);
  EXPECT_EQ(take(invoke("WdfRequestRequeue", {Globals, First})), 0u);
  expectError(invoke("WdfRequestComplete", {Globals, First, 0}),
              "framework owns");
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), First);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  EXPECT_EQ(get(BufferSlot), Second);
  complete(Second, 2);
  complete(First, 1);
}

TEST_F(DriverKernelFrameworkRequest,
       RequeuedAlreadyCanceledRequestRunsFrameworkCleanup) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            0u);
  Packets.at(IRP).Canceled = true;
  EXPECT_EQ(take(invoke("WdfRequestRequeue", {Globals, Request})), 0u);
  auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::RequestCancelled);
  auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(BufferSlot), 0u);
}

TEST_F(DriverKernelFrameworkRequest,
       FrameworkCancelsQueuedManualRequestWithoutDriverDelivery) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP)));
  Packets.at(IRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::RequestCancelled);
  EXPECT_EQ(Packets.at(IRP).Information, 0u);
  EXPECT_TRUE(Released.count(Request));
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(BufferSlot), 0u);
}

TEST_F(DriverKernelFrameworkRequest,
       QueuedManualCancellationRunsCleanupBeforeRetiringTheIRP) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP)));
  Packets.at(IRP).Canceled = true;
  auto Cleanup = take(Model.requestCancellation(IRP));
  ASSERT_TRUE(Cleanup);
  EXPECT_EQ(Cleanup->PC, ChildCleanup);
  EXPECT_EQ(Cleanup->Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(get(Context), Sentinel);
  expectError(invoke("WdfRequestComplete", {Globals, Request, 0}),
              "completion in progress");
  finish(*Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::RequestCancelled);
  auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       AlreadyCanceledRequestIsRetiredWhenForwardedToManualQueue) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  Packets.at(IRP).Canceled = true;
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::RequestCancelled);
  put(BufferSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, BufferSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(BufferSlot), 0u);
}

TEST_F(DriverKernelFrameworkRequest,
       AlreadyCanceledForwardRunsNestedRequestCleanup) {
  initializeQueue();
  const auto Manual = manualQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  Packets.at(IRP).Canceled = true;
  EXPECT_EQ(
      take(invoke("WdfRequestForwardToIoQueue", {Globals, Request, Manual})),
      0u);
  auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(get(Context), Sentinel);
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).Status, framework::RequestCancelled);
  auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       ParallelQueueDeliversIndependentRequestsBeforeEitherCompletes) {
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, UINT32_MAX, 4);
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto First = request(route(FirstIRP));
  const auto Second = request(route(SecondIRP));
  EXPECT_NE(First, Second);
  EXPECT_EQ(Packets.at(FirstIRP).PendingCalls, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).PendingCalls, 1u);
  complete(Second, 2);
  EXPECT_FALSE(Packets.at(FirstIRP).Completed);
  complete(First, 1);
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).Information, 2u);
}

TEST_F(DriverKernelFrameworkRequest,
       BoundedParallelQueuePresentsNextRequestAfterCompletion) {
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, 1, 4);
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto ThirdIRP = packet();
  const auto First = request(route(FirstIRP));
  auto Second = route(SecondIRP);
  auto Third = route(ThirdIRP);
  EXPECT_EQ(Second.PC, 0u);
  EXPECT_EQ(Second.Status, 0x103u);
  EXPECT_EQ(Third.PC, 0u);
  EXPECT_EQ(Packets.at(SecondIRP).PendingCalls, 1u);
  EXPECT_EQ(Packets.at(ThirdIRP).PendingCalls, 1u);
  complete(First, 1);
  auto SecondCall = callback();
  EXPECT_EQ(SecondCall.PC, IoControlPC);
  ASSERT_GE(SecondCall.Arguments.size(), 2u);
  EXPECT_EQ(SecondCall.Arguments[0], Queue);
  const auto SecondHandle = SecondCall.Arguments[1];
  EXPECT_NE(SecondHandle, First);
  EXPECT_FALSE(Packets.at(SecondIRP).Completed);
  finish(SecondCall);
  complete(SecondHandle, 2);
  auto ThirdCall = callback();
  EXPECT_EQ(ThirdCall.PC, IoControlPC);
  ASSERT_GE(ThirdCall.Arguments.size(), 2u);
  const auto ThirdHandle = ThirdCall.Arguments[1];
  EXPECT_NE(ThirdHandle, SecondHandle);
  finish(ThirdCall);
  complete(ThirdHandle, 3);
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(SecondIRP).Information, 2u);
  EXPECT_EQ(Packets.at(ThirdIRP).Information, 3u);
}

TEST_F(DriverKernelFrameworkRequest,
       CanceledBoundedParallelWaiterNeverReachesDriver) {
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, 1, 4);
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto ThirdIRP = packet();
  const auto First = request(route(FirstIRP));
  EXPECT_EQ(route(SecondIRP).PC, 0u);
  EXPECT_EQ(route(ThirdIRP).PC, 0u);
  Packets.at(SecondIRP).Canceled = true;
  EXPECT_TRUE(take(Model.preflightRequestCancellation(SecondIRP)));
  EXPECT_FALSE(take(Model.requestCancellation(SecondIRP)));
  EXPECT_TRUE(Packets.at(SecondIRP).Completed);
  EXPECT_EQ(Packets.at(SecondIRP).Status, framework::RequestCancelled);
  complete(First);
  auto ThirdCall = callback();
  EXPECT_EQ(ThirdCall.PC, IoControlPC);
  ASSERT_GE(ThirdCall.Arguments.size(), 2u);
  finish(ThirdCall);
  complete(ThirdCall.Arguments[1]);
  EXPECT_TRUE(Packets.at(ThirdIRP).Completed);
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkRequest,
       BoundedParallelLimitTwoPreservesAnotherPresentedRequest) {
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, 2, 4);
  initializeQueue();
  const auto FirstIRP = packet();
  const auto SecondIRP = packet();
  const auto ThirdIRP = packet();
  const auto First = request(route(FirstIRP));
  const auto Second = request(route(SecondIRP));
  EXPECT_EQ(route(ThirdIRP).PC, 0u);
  complete(Second, 2);
  auto Third = callback();
  EXPECT_EQ(Third.PC, IoControlPC);
  ASSERT_GE(Third.Arguments.size(), 2u);
  EXPECT_FALSE(Packets.at(FirstIRP).Completed);
  finish(Third);
  complete(Third.Arguments[1], 3);
  complete(First, 1);
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_EQ(Packets.at(FirstIRP).Information, 1u);
  EXPECT_EQ(Packets.at(ThirdIRP).Information, 3u);
}

TEST_F(DriverKernelFrameworkRequest,
       CleanupPrecedesIRPRetirementButReferenceKeepsOnlyContext) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  take(invoke("WdfObjectAllocateContext",
              {Globals, Request, Attrs, ContextOutput}));
  const auto Context = get(ContextOutput);
  put(Context, Sentinel);
  take(invoke("WdfObjectReferenceActual", {Globals, Request, 0, 0, 0}));
  EXPECT_EQ(retrieve(Request, true), 0u);
  const auto Buffer = get(BufferSlot);
  put(Buffer, Sentinel);
  complete(Request, 7);
  const auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_EQ(Cleanup.Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 0u);
  expectError(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "completion in progress");
  expectError(invoke("WdfRequestRetrieveInputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "completion in progress");
  EXPECT_EQ(get(Buffer), Sentinel);
  put(Buffer, Sentinel - 1);
  EXPECT_EQ(get(Buffer), Sentinel - 1);
  put(Parameters, 40, 2);
  expectError(invoke("WdfRequestGetParameters", {Globals, Request, Parameters}),
              "completion in progress");
  expectError(
      invoke("WdfRequestCompleteWithInformation", {Globals, Request, 0, 0}),
      "complet");
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_EQ(Packets.at(IRP).Information, 7u);
  EXPECT_FALSE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
  expectError(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "completed framework request");
  expectError(invoke("WdfRequestComplete", {Globals, Request, 0}),
              "completed framework request");
  success(Model.validateGuestAccess(Context, 24, false));
  EXPECT_EQ(get(Context), Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectContextGetObject", {Globals, Context})),
            Request);
  // A retained completed object must not block the sequential queue.
  const auto NextIRP = packet();
  const auto Next = request(route(NextIRP));
  complete(Next);
  take(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  EXPECT_EQ(get(Context), Sentinel);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
  expectError(Model.validateGuestAccess(Context, 1, false), "freed");
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       CompletionPreflightFailurePreservesTheLiveRequest) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  take(invoke("WdfObjectAllocateContext",
              {Globals, Request, Attrs, ContextOutput}));
  const auto Context = get(ContextOutput);
  put(Context, Sentinel);
  Packets.at(IRP).Information = 2;
  const auto Attempts = AllocationAttempts;
  expectError(invoke("WdfRequestComplete", {Globals, Request, 0x103}),
              "STATUS_PENDING");
  expectError(
      invoke("WdfRequestCompleteWithInformation", {Globals, Request, 0x103, 3}),
      "STATUS_PENDING");
  FailValidation = true;
  expectError(
      invoke("WdfRequestCompleteWithInformation", {Globals, Request, 0, 5}),
      "injected validation failure");
  FailValidation = false;
  EXPECT_EQ(Validations, (std::vector<std::tuple<uint64_t, uint32_t, uint64_t>>{
                             {IRP, 0x103, 2}, {IRP, 0x103, 3}, {IRP, 0, 5}}));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 0u);
  EXPECT_EQ(Packets.at(IRP).Information, 2u);
  EXPECT_EQ(Packets.at(IRP).Status, 0u);
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_FALSE(Released.count(Request));
  EXPECT_EQ(get(Context), Sentinel);
  EXPECT_EQ(retrieve(Request, true), 0u);
  // The failed preflight cannot mark the object as deleting: context allocation
  // remains valid and must not return STATUS_DELETE_PENDING.
  type(TypeOther);
  attributes(0, TypeOther);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Request, Attrs, ContextOutput})),
            0u);
  complete(Request, 4);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_EQ(Packets.at(IRP).Information, 4u);
  EXPECT_EQ(Validations.back(), std::make_tuple(IRP, uint32_t(0), uint64_t(4)));
}

TEST_F(DriverKernelFrameworkRequest,
       RequestChildDestructionPrecedesHostCompletion) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  type();
  attributes(0, Type, ParentCleanup, ParentDestroy);
  take(invoke("WdfObjectAllocateContext",
              {Globals, Request, Attrs, ContextOutput}));
  attributes(Request, Type, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  EXPECT_EQ(retrieve(Request, true), 0u);
  const auto Buffer = get(BufferSlot);
  put(Buffer, Sentinel);
  complete(Request, 7);
  auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Child}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ParentCleanup);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ChildDestroy);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Child}));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(get(Buffer), Sentinel);
  HostEvents.push_back("child destroy");
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ParentDestroy);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_TRUE(Released.count(Child));
  EXPECT_FALSE(Released.count(Request));
  HostEvents.push_back("request destroy");
  finish(Call);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(HostEvents,
            (std::vector<std::string>{"pending", "child destroy", "complete",
                                      "request destroy"}));
}

TEST_F(DriverKernelFrameworkRequest,
       PendingRequestPreventsParentDeletionWithoutMutatingOwnership) {
  initializeQueue();
  type();
  attributes(0, Type, ParentCleanup, ParentDestroy);
  take(invoke("WdfObjectAllocateContext",
              {Globals, Device, Attrs, ContextOutput}));
  const auto DeviceContext = get(ContextOutput);
  put(DeviceContext, Sentinel);
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Attempts = AllocationAttempts;
  const auto Releases = Released.size();
  expectError(invoke("WdfObjectDelete", {Globals, Device}), "live request");
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(Released.size(), Releases);
  EXPECT_TRUE(HostDevices.count(WdmDevice));
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), Queue);
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Queue})), Device);
  EXPECT_EQ(get(DeviceContext), Sentinel);
  EXPECT_EQ(retrieve(Request, false), 0u);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 0u);
  // A new child proves preflight did not mark the device as deleting.
  attributes(Device);
  const auto Child = object(Attrs);
  complete(Request);
  take(invoke("WdfObjectDelete", {Globals, Device}));
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ParentCleanup, ParentDestroy}));
  EXPECT_TRUE(Released.count(Child));
  EXPECT_TRUE(Released.count(Queue));
  EXPECT_TRUE(Released.count(Device));
  EXPECT_TRUE(HostDevices.empty());
}

TEST_F(DriverKernelFrameworkRequest, SuccessfulUnmarkWinsAndReleasesItsHold) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  EXPECT_EQ(take(invoke("WdfRequestIsCanceled", {Globals, Request})), 0u);
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000010u);
  expectError(invoke("WdfRequestMarkCancelableEx", {Globals, Request, 0}),
              "cancel callback");
  markCancelable(Request);
  EXPECT_EQ(
      take(invoke("WdfRequestMarkCancelableEx", {Globals, Request, CancelPC})),
      0xc0000010u);
  expectError(invoke("WdfRequestIsCanceled", {Globals, Request}), "unmarked");
  expectError(invoke("WdfRequestComplete", {Globals, Request, 0}),
              "UnmarkCancelable");
  EXPECT_TRUE(Validations.empty());
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})), 0u);
  // A second registration proves successful unmark restored the initial state.
  markCancelable(Request);
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})), 0u);
  Packets.at(IRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_EQ(take(invoke("WdfRequestIsCanceled", {Globals, Request})), 1u);
  complete(Request, 0, 0xc0000120);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       AlreadyCanceledMarkExNeverRegistersOrCallsTheCallback) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  Packets.at(IRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_EQ(
      take(invoke("WdfRequestMarkCancelableEx", {Globals, Request, CancelPC})),
      0xc0000120u);
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_EQ(take(invoke("WdfRequestIsCanceled", {Globals, Request})), 1u);
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000010u);
  complete(Request, 0, 0xc0000120);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       QueuedCancellationMustEnterBeforeEitherParticipantCompletes) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  markCancelable(Request);
  expectError(Model.requestCancellation(IRP), "IRP cancel flag");
  const auto Cancel = cancel(IRP);
  EXPECT_EQ(Cancel.PC, CancelPC);
  EXPECT_EQ(Cancel.Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  expectError(Model.finishGuestCall(Cancel.Token, 0), "has not entered");
  expectError(Model.beginCancelCallback(Cancel.Token + 1), "unknown");
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000120u);
  expectError(invoke("WdfRequestComplete", {Globals, Request, 0xc0000120}),
              "delivered");
  EXPECT_TRUE(Validations.empty());
  success(Model.beginCancelCallback(Cancel.Token));
  expectError(Model.beginCancelCallback(Cancel.Token), "already delivered");
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000120u);
  EXPECT_EQ(
      take(invoke("WdfRequestMarkCancelableEx", {Globals, Request, CancelPC})),
      0xc0000010u);
  // A cooperating worker can make this call while the cancel frame waits.
  complete(Request, 0, 0xc0000120);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_FALSE(Released.count(Request));
  expectError(invoke("WdfRequestUnmarkCancelable", {Globals, Request}),
              "completed framework request");
  expectError(
      invoke("WdfRequestMarkCancelableEx", {Globals, Request, CancelPC}),
      "completed framework request");
  expectError(invoke("WdfRequestIsCanceled", {Globals, Request}),
              "completed framework request");
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  expectError(Model.finishGuestCall(Cancel.Token, 0), "invalid");
  expectError(Model.beginCancelCallback(Cancel.Token), "unknown");
}

TEST_F(DriverKernelFrameworkRequest,
       CancelCompletionCleansImmediatelyButDestroysAfterCallbackReturn) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  markCancelable(Request);
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  const auto Cleanup = callback();
  EXPECT_NE(Cleanup.Token, Cancel.Token);
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  expectError(invoke("WdfRequestIsCanceled", {Globals, Request}),
              "completion in progress");
  expectError(invoke("WdfRequestUnmarkCancelable", {Globals, Request}),
              "completion in progress");
  expectError(
      invoke("WdfRequestMarkCancelableEx", {Globals, Request, CancelPC}),
      "completion in progress");
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_FALSE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_EQ(get(Context), Sentinel);
  // The parent may not be synchronously deleted under a suspended callback.
  expectError(invoke("WdfObjectDelete", {Globals, Device}),
              "outstanding cancellation callback");
  EXPECT_FALSE(take(Model.finishGuestCall(Cancel.Token, 0)));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.Token, Cancel.Token);
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  EXPECT_EQ(Destroy.Arguments, (std::vector<uint64_t>{Request}));
  EXPECT_FALSE(Released.count(Request));
  EXPECT_EQ(get(Context), Sentinel);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
  EXPECT_TRUE(Released.count(Context));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       ExplicitDereferenceCannotConsumeTheCancellationHold) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  requestContext(Request);
  markCancelable(Request);
  expectError(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}),
              "underflow");
  take(invoke("WdfObjectReferenceActual", {Globals, Request, 0, 0, 0}));
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  const auto Cleanup = callback();
  finish(Cleanup);
  take(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Released.count(Request));
  expectError(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}),
              "underflow");
  EXPECT_FALSE(take(Model.finishGuestCall(Cancel.Token, 0)));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       ExplicitReferenceOutlivesTheCancelCallbackAndItsInternalHold) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  markCancelable(Request);
  take(invoke("WdfObjectReferenceActual", {Globals, Request, 0, 0, 0}));
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  finish(callback());
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Released.count(Context));
  EXPECT_FALSE(Released.count(Request));
  // A retained completed request does not hold sequential queue ownership.
  const auto Next = request(route(packet()));
  complete(Next);
  take(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}));
  const auto Destroy = callback();
  EXPECT_NE(Destroy.Token, Cancel.Token);
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       CancelCallbackMayReturnBeforeCooperatingWorkerCompletes) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  requestContext(Request);
  markCancelable(Request);
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_FALSE(Released.count(Request));
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000120u);
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationCannotReplaceAnotherPendingFrameworkCallback) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  markCancelable(Request);
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  const auto Other = object(Attrs);
  take(invoke("WdfObjectDelete", {Globals, Other}));
  Packets.at(IRP).Canceled = true;
  expectError(Model.requestCancellation(IRP), "pending guest callback");
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  const auto Cancel = take(Model.requestCancellation(IRP));
  ASSERT_TRUE(Cancel);
  EXPECT_EQ(Cancel->PC, CancelPC);
  EXPECT_FALSE(Model.takeGuestCall());
  success(Model.beginCancelCallback(Cancel->Token));
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(take(Model.finishGuestCall(Cancel->Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       LegacyAlreadyCanceledMarkRetainsNestedCompletionAndDestroyContinuation) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const auto Context = requestContext(Request);
  put(Context, Sentinel);
  Packets.at(IRP).Canceled = true;
  const auto Entry = entry("WdfRequestMarkCancelable");
  EXPECT_EQ(KernelFramework::argumentCount(Entry), 3u);
  EXPECT_EQ(get(get(TableSlot) + 255 * 8), Entry.Address);

  take(Model.call(Entry, {Globals, Request, CancelPC}, 0));
  const auto Cancel = callback();
  EXPECT_EQ(Cancel.PC, CancelPC);
  EXPECT_EQ(Cancel.Arguments, (std::vector<uint64_t>{Request}));
  // This child is delivered by the current API continuation, without waiting
  // for a scheduler entry. The session has not yet returned the void API.
  expectError(Model.beginCancelCallback(Cancel.Token), "already delivered");
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})),
            0xc0000120u);
  expectError(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}),
              "underflow");

  // A worker may complete while this delivered cancellation frame is suspended.
  // The model test holds each continuation open; guest waits are tested by the
  // session fixture that executes these callbacks.
  complete(Request, 0, 0xc0000120);
  const auto Cleanup = callback();
  EXPECT_NE(Cleanup.Token, Cancel.Token);
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Packets.at(IRP).Completed);
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}),
              "completion in progress");
  finish(Cleanup);
  EXPECT_TRUE(Packets.at(IRP).Completed);
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_FALSE(Released.count(Request));
  success(Model.validateGuestAccess(Context, 8, false));
  EXPECT_EQ(get(Context), Sentinel);
  expectError(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request, 0, BufferSlot, LengthSlot}),
              "completed framework request");

  // Returning from EvtRequestCancel must still leave the original API pending
  // while final destruction runs, including any wait by the destroy callback.
  EXPECT_FALSE(take(Model.finishGuestCall(Cancel.Token, Sentinel)));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.Token, Cancel.Token);
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  EXPECT_FALSE(Released.count(Context));
  EXPECT_EQ(get(Context), Sentinel);
  EXPECT_EQ(take(Model.finishGuestCall(Destroy.Token, Sentinel)),
            std::optional<uint64_t>{0});
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_TRUE(Released.count(Request));
  EXPECT_TRUE(Released.count(Context));
  expectError(Model.validateGuestAccess(Context, 1, false), "freed");
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}),
              "completed framework request");
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       LegacyCancelCallbackMayReturnBeforeLaterRequestCompletion) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  requestContext(Request);
  Packets.at(IRP).Canceled = true;
  take(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}));
  const auto Cancel = callback();
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, Sentinel)),
            std::optional<uint64_t>{0});
  EXPECT_FALSE(Packets.at(IRP).Completed);
  EXPECT_FALSE(Released.count(Request));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}),
              "unmarked request");
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       LegacyNormalMarkSupportsUnmarkAndDeferredCancellation) {
  initializeQueue();
  const auto FirstIRP = packet();
  const auto First = request(route(FirstIRP));
  take(invoke("WdfRequestMarkCancelable", {Globals, First, CancelPC}));
  EXPECT_FALSE(Model.takeGuestCall());
  expectError(invoke("WdfRequestComplete", {Globals, First, 0}),
              "UnmarkCancelable");
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, First})), 0u);
  Packets.at(FirstIRP).Canceled = true;
  EXPECT_FALSE(take(Model.requestCancellation(FirstIRP)));
  complete(First);
  EXPECT_EQ(Packets.at(FirstIRP).CompletionCalls, 1u);
  EXPECT_TRUE(Released.count(First));

  const auto SecondIRP = packet();
  const auto Second = request(route(SecondIRP));
  take(invoke("WdfRequestMarkCancelable", {Globals, Second, CancelPC}));
  EXPECT_FALSE(Model.takeGuestCall());
  const auto Cancel = cancel(SecondIRP);
  EXPECT_EQ(Cancel.PC, CancelPC);
  expectError(invoke("WdfRequestComplete", {Globals, Second, 0xc0000120}),
              "delivered");
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Second, 0, 0xc0000120);
  EXPECT_FALSE(Released.count(Second));
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Second));
  EXPECT_EQ(Packets.at(SecondIRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       LegacyInvalidMarksAreErrorsWithoutChangingCancellationOwnership) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, 0}),
              "cancel callback");
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Sentinel, CancelPC}),
              "invalid, foreign");
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Queue, CancelPC}),
              "invalid, foreign");
  expectError(
      invoke("WdfRequestMarkCancelable", {Globals + 8, Request, CancelPC}),
      "another binding's globals");
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_TRUE(Validations.empty());
  EXPECT_EQ(take(invoke("WdfRequestIsCanceled", {Globals, Request})), 0u);
  expectError(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}),
              "underflow");

  take(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}));
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, DefaultPC}),
              "unmarked request");
  const auto Cancel = cancel(IRP);
  EXPECT_EQ(Cancel.PC, CancelPC);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       LegacyImmediateCancellationPreflightsPendingGuestCall) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  const auto Other = object(Attrs);
  take(invoke("WdfObjectDelete", {Globals, Other}));
  Packets.at(IRP).Canceled = true;
  expectError(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}),
              "pending guest callback");
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  EXPECT_TRUE(Released.count(Other));
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  EXPECT_EQ(take(invoke("WdfRequestIsCanceled", {Globals, Request})), 1u);
  // A retry after the unrelated callback completes must still be a first mark.
  take(invoke("WdfRequestMarkCancelable", {Globals, Request, CancelPC}));
  const auto Cancel = callback();
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
  EXPECT_EQ(Packets.at(IRP).CompletionCalls, 1u);
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationPreviewCountsOnlyMarkedRequestsWithoutReadingHostFacts) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  const KernelFramework &Preview = Model;
  EXPECT_FALSE(take(Preview.preflightRequestCancellation(Sentinel)));
  EXPECT_FALSE(take(Preview.preflightRequestCancellation(IRP)));
  markCancelable(Request);
  const auto Queries = CancelQueries;
  const auto Events = HostEvents;
  const auto Bytes = Memory.Bytes;
  for (unsigned I = 0; I < 3; ++I) {
    EXPECT_TRUE(take(Preview.preflightRequestCancellation(IRP)));
    EXPECT_FALSE(Model.hasPendingGuestCall());
  }
  EXPECT_EQ(CancelQueries, Queries);
  EXPECT_EQ(HostEvents, Events);
  EXPECT_EQ(Memory.Bytes, Bytes);
  EXPECT_FALSE(Packets.at(IRP).Canceled);
  expectError(Model.requestCancellation(IRP), "IRP cancel flag");
  EXPECT_TRUE(take(Preview.preflightRequestCancellation(IRP)));
  // Repeated previews did not publish a call or consume the cancellation hold.
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})), 0u);
  EXPECT_FALSE(take(Preview.preflightRequestCancellation(IRP)));
  complete(Request);
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationPreviewMatchesQueuedDeliveredAndCompletedCallbackCounts) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  markCancelable(Request);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP)));
  const auto Cancel = cancel(IRP);
  EXPECT_EQ(Cancel.PC, CancelPC);
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP)));
  EXPECT_FALSE(take(Model.requestCancellation(IRP)));
  success(Model.beginCancelCallback(Cancel.Token));
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP)));
  complete(Request, 0, 0xc0000120);
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP)));
  EXPECT_FALSE(Released.count(Request));
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP)));
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationPreviewRejectsPendingCallWithoutConsumingEitherOwner) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  markCancelable(Request);
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  const auto Other = object(Attrs);
  take(invoke("WdfObjectDelete", {Globals, Other}));
  const auto Queries = CancelQueries;
  expectError(Model.preflightRequestCancellation(IRP),
              "pending guest callback");
  expectError(Model.preflightRequestCancellation(IRP),
              "pending guest callback");
  EXPECT_EQ(CancelQueries, Queries);
  EXPECT_TRUE(Model.hasPendingGuestCall());
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  EXPECT_TRUE(Released.count(Other));
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP)));
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationPreviewRejectsMissingHostOnlyForAnActualCallback) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  markCancelable(Request);
  Model.setRequestHost({});
  EXPECT_FALSE(take(Model.preflightRequestCancellation(Sentinel)));
  expectError(Model.preflightRequestCancellation(IRP), "host is unavailable");
  expectError(Model.requestCancellation(IRP), "host is unavailable");
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_EQ(take(invoke("WdfRequestUnmarkCancelable", {Globals, Request})), 0u);
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP)));
}

TEST_F(DriverKernelFrameworkRequest,
       CancellationPreviewChecksBatchTokenCapacityWithoutReservingTokens) {
  initializeQueue();
  const auto IRP = packet();
  const auto Request = request(route(IRP));
  EXPECT_FALSE(take(Model.preflightRequestCancellation(IRP, UINT64_MAX)));
  markCancelable(Request);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP, 0)));
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP, 7)));
  expectError(Model.preflightRequestCancellation(IRP, UINT64_MAX),
              "token capacity exhausted");
  expectError(Model.preflightRequestCancellation(IRP, UINT64_MAX - 1),
              "token capacity exhausted");
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_FALSE(Packets.at(IRP).Canceled);
  EXPECT_TRUE(take(Model.preflightRequestCancellation(IRP)));
  const auto Cancel = cancel(IRP);
  success(Model.beginCancelCallback(Cancel.Token));
  complete(Request, 0, 0xc0000120);
  EXPECT_EQ(take(Model.finishGuestCall(Cancel.Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Released.count(Request));
}

} // namespace
} // namespace neverd::emulation

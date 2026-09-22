//===- KernelPnpRequestTests.cpp - PnP packet lifetime -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check explicit provider responses and lifecycle ownership at WDM boundaries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint32_t StatusDeletePending = 0xc0000056;

class KernelPnpRequest : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t PDO = 0, FDO = 0;

  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return T{};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    Options.PnpDevices.push_back({"sensor0", DriverBusKind::ResourceFree,
                                  DevicePowerState::D0,
                                  SystemPowerState::Working});
    Result.Configuration = Options;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u, 27u}) {
      const uint64_t Address =
          Model->driverObject() + DriverDispatchOffset + Major * 8;
      ok(Model->validateGuestAccess(Address, 8, true));
      put(Address, Entry);
    }
    const auto Extension = get(Model->driverObject() + DriverExtensionOffset);
    ok(Model->validateGuestAccess(Extension + DriverAddDeviceOffset, 8, true));
    put(Extension + DriverAddDeviceOffset, Entry + 0x100);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    auto Add = take(Model->beginAddDevice("sensor0"));
    EXPECT_EQ(Add.Argument0, Model->driverObject());
    PDO = Add.Argument1;
    ASSERT_NE(PDO, 0u);
  }
  void attach(llvm::StringRef Name = {}) {
    uint64_t NameRecord = 0;
    if (!Name.empty()) {
      NameRecord = Scratch + 0x100;
      std::vector<uint8_t> Bytes((Name.size() + 1) * 2);
      for (size_t I = 0; I < Name.size(); ++I)
        Bytes[I * 2] = Name[I];
      ok(Memory->write(NameRecord + 0x20, Bytes));
      put(NameRecord, Name.size() * 2, 2);
      put(NameRecord + 2, Bytes.size(), 2);
      put(NameRecord + 8, NameRecord + 0x20);
    }
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, NameRecord,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    ok(Model->finishAddDevice("sensor0", StatusSuccess));
  }
  DriverRequest pnp(DevicePnpRequest Minor, uint32_t Status = StatusSuccess,
                    uint64_t Delay = 0) {
    DriverRequest Input;
    Input.Kind = DriverRequestKind::Pnp;
    Input.DeviceID = "sensor0";
    Input.Pnp = DriverPnpOperation{Minor, {Status, Delay}};
    return Input;
  }
  KernelModel::Invocation begin(DevicePnpRequest Minor,
                                uint32_t Status = StatusSuccess,
                                uint64_t Delay = 0) {
    return take(Model->beginRequest(pnp(Minor, Status, Delay)));
  }
  uint32_t forward(uint64_t IRP, uint64_t Completion = 0) {
    const auto Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Memory->read(Stack, Prefix));
    ok(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset,
        Completion ? StackInvokeOnSuccess | StackInvokeOnError : 0, 1);
    put(Stack - StackSize + StackCompletionOffset, Completion);
    return call("IofCallDriver", {PDO, IRP});
  }
  void status(uint64_t IRP, uint32_t Value, uint64_t Information = 0) {
    ok(Model->validateGuestAccess(IRP + IRPStatusOffset, 16, true));
    put(IRP + IRPStatusOffset, Value, 4);
    put(IRP + IRPInformationOffset, Information);
  }
  void finish(uint64_t IRP, uint32_t Dispatch = StatusSuccess) {
    ok(Model->recordDispatchReturn(IRP, Dispatch));
    ok(Model->finalizeRequest(IRP));
  }
  void start() {
    const auto Request = begin(DevicePnpRequest::Start);
    EXPECT_EQ(forward(Request.IRP), StatusSuccess);
    finish(Request.IRP);
  }
  KernelModel::Invocation file(DriverRequestKind Kind) {
    DriverRequest Input;
    Input.Kind = Kind;
    if (Kind == DriverRequestKind::Create)
      Input.DeviceID = "sensor0";
    return take(Model->beginRequest(Input));
  }
  void completeFile(DriverRequestKind Kind, uint32_t Status = StatusSuccess) {
    const auto Invocation = file(Kind);
    EXPECT_EQ(Invocation.PC, Entry);
    EXPECT_EQ(Invocation.Argument0, FDO);
    status(Invocation.IRP, Status);
    call("IofCompleteRequest", {Invocation.IRP, 0});
    finish(Invocation.IRP, Status);
  }
  void completePnp(DevicePnpRequest Minor) {
    const auto Invocation = begin(Minor);
    EXPECT_EQ(forward(Invocation.IRP), StatusSuccess);
    finish(Invocation.IRP);
  }
};

TEST_F(KernelPnpRequest, StartPacketIsKernelModeWithoutFileOrResources) {
  attach();
  const auto Request = begin(DevicePnpRequest::Start);
  EXPECT_EQ(Request.PC, Entry);
  EXPECT_EQ(Request.Argument0, FDO);
  const auto IRP = Request.IRP;
  const auto Stack = get(IRP + IRPStackPointerOffset);
  EXPECT_EQ(get(IRP + IRPOriginalFileOffset), 0u);
  EXPECT_EQ(get(IRP + IRPRequestorModeOffset, 1), KernelMode);
  EXPECT_EQ(get(IRP + IRPFlagsOffset, 4), 0u);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusNotSupported);
  EXPECT_EQ(get(Stack + StackFileOffset), 0u);
  EXPECT_EQ(get(Stack + StackStartResourcesOffset), 0u);
  EXPECT_EQ(get(Stack + StackStartTranslatedResourcesOffset), 0u);
  EXPECT_EQ(get(Stack + StackMinorOffset, 1), uint8_t(DevicePnpRequest::Start));
  EXPECT_EQ(forward(IRP), StatusSuccess);
  finish(IRP);
  ASSERT_TRUE(Result.Requests.back().Pnp);
  EXPECT_EQ(Result.Requests.back().Pnp->StateBefore,
            DevicePnpState::NotStarted);
  EXPECT_EQ(Result.Requests.back().Pnp->StateAfter, DevicePnpState::Started);
  EXPECT_EQ(Result.Requests.back().Pnp->BusStatus, StatusSuccess);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Started);
}

TEST_F(KernelPnpRequest, LocalStartFailureDoesNotConsumeConfiguredBusResponse) {
  attach();
  const auto Request = begin(DevicePnpRequest::Start, StatusSuccess, 25);
  status(Request.IRP, StatusNotSupported);
  call("IofCompleteRequest", {Request.IRP, 0});
  finish(Request.IRP, StatusNotSupported);
  const auto &Pnp = *Result.Requests.back().Pnp;
  EXPECT_EQ(Pnp.StateAfter, DevicePnpState::NotStarted);
  EXPECT_FALSE(Pnp.BusReceivedAt100ns);
  EXPECT_FALSE(Pnp.BusCompletedAt100ns);
  EXPECT_FALSE(Pnp.BusStatus);
  EXPECT_FALSE(Model->nextEventTime());
}

TEST_F(KernelPnpRequest, LocalSuccessfulStartCannotSkipTheProvider) {
  attach();
  const auto Request = begin(DevicePnpRequest::Start);
  const auto Cursor = get(Request.IRP + IRPStackPointerOffset);
  status(Request.IRP, StatusSuccess);
  auto Completion = Model->call("IofCompleteRequest", {Request.IRP, 0});
  ASSERT_FALSE(bool(Completion));
  reject(Completion.takeError(), "requires actual provider completion");
  EXPECT_EQ(get(Request.IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusReceivedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusCompletedAt100ns);
  ok(Model->snapshot());
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::NotStarted);
}

TEST_F(KernelPnpRequest, LocalQueryFailureDoesNotConsumeConfiguredResponse) {
  attach();
  start();
  const auto Request = begin(DevicePnpRequest::QueryRemove, StatusSuccess, 25);
  status(Request.IRP, StatusNotSupported);
  call("IofCompleteRequest", {Request.IRP, 0});
  finish(Request.IRP, StatusNotSupported);
  EXPECT_EQ(Result.Requests.back().Pnp->StateAfter, DevicePnpState::Started);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusReceivedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusCompletedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusStatus);
  EXPECT_FALSE(Model->nextEventTime());
}

TEST_F(KernelPnpRequest, NonzeroSuccessfulRemoveCannotCommitLifecycle) {
  attach();
  const auto Request = begin(DevicePnpRequest::Remove);
  EXPECT_EQ(forward(Request.IRP, Entry + 0x200), StatusSuccess);
  const auto Callback = Model->takeGuestCall();
  ASSERT_TRUE(Callback);
  const auto Cursor = get(Request.IRP + IRPStackPointerOffset);
  status(Request.IRP, 1);
  auto Completion = Model->finishGuestCall(Callback->Token, 0);
  ASSERT_FALSE(bool(Completion));
  reject(Completion.takeError(), "STATUS_SUCCESS");
  EXPECT_EQ(get(Request.IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Pnp->BusStatus, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().Pnp->BusCompletedAt100ns, 0u);
  ok(Model->snapshot());
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Removing);
}

TEST_F(KernelPnpRequest, NonzeroSuccessfulCancelRemoveCannotRestoreLifecycle) {
  attach();
  const auto Query = begin(DevicePnpRequest::QueryRemove);
  forward(Query.IRP);
  finish(Query.IRP);
  const auto Request = begin(DevicePnpRequest::CancelRemove);
  EXPECT_EQ(forward(Request.IRP, Entry + 0x200), StatusSuccess);
  const auto Callback = Model->takeGuestCall();
  ASSERT_TRUE(Callback);
  const auto Cursor = get(Request.IRP + IRPStackPointerOffset);
  status(Request.IRP, 1);
  auto Completion = Model->finishGuestCall(Callback->Token, 0);
  ASSERT_FALSE(bool(Completion));
  reject(Completion.takeError(), "STATUS_SUCCESS");
  EXPECT_EQ(get(Request.IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(Result.Requests.back().Completed);
  ok(Model->snapshot());
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::RemovePending);
}

TEST_F(KernelPnpRequest, UpperFinalStatusOwnsQueryRollback) {
  attach();
  start();
  const auto Query = begin(DevicePnpRequest::QueryRemove);
  EXPECT_EQ(forward(Query.IRP, Entry + 0x200), 0u);
  auto Callback = Model->takeGuestCall();
  ASSERT_TRUE(Callback);
  status(Query.IRP, StatusNotSupported);
  auto Finished = take(Model->finishGuestCall(Callback->Token, 0));
  ASSERT_TRUE(Finished);
  EXPECT_EQ(*Finished, StatusSuccess);
  finish(Query.IRP, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().IOStatus, StatusNotSupported);
  EXPECT_EQ(Result.Requests.back().Pnp->BusStatus, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().Pnp->StateAfter, DevicePnpState::Started);
  completeFile(DriverRequestKind::Create);
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
}

TEST_F(KernelPnpRequest, GuestDispatchOwnsCreateFailuresAndCancelReopensFile) {
  attach();
  // NotStarted does not synthesize a dispatch decision. A real guest failure
  // must retire the opening file identity so the same ID can be used again.
  completeFile(DriverRequestKind::Create, StatusNotSupported);
  EXPECT_EQ(Result.Requests.back().IOStatus, StatusNotSupported);
  start();
  completePnp(DevicePnpRequest::QueryRemove);
  completeFile(DriverRequestKind::Create, StatusDeletePending);
  EXPECT_EQ(Result.Requests.back().IOStatus, StatusDeletePending);
  completePnp(DevicePnpRequest::CancelRemove);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Started);
  completeFile(DriverRequestKind::Create);
  EXPECT_EQ(Result.Requests.back().DeviceID, "sensor0");
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
}

TEST_F(KernelPnpRequest, LiveFilesDispatchAcrossPauseQueryAndSurpriseStates) {
  attach();
  start();
  completeFile(DriverRequestKind::Create);
  for (auto Minor : {DevicePnpRequest::QueryStop, DevicePnpRequest::CancelStop,
                     DevicePnpRequest::QueryStop, DevicePnpRequest::Stop,
                     DevicePnpRequest::Start, DevicePnpRequest::QueryRemove,
                     DevicePnpRequest::CancelRemove}) {
    completePnp(Minor);
    // A software-only IOCTL can succeed even while the device is paused.
    completeFile(DriverRequestKind::DeviceControl);
    EXPECT_EQ(Result.Requests.back().IOStatus, StatusSuccess);
  }
  completePnp(DevicePnpRequest::SurpriseRemoval);
  ASSERT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::SurpriseRemoved);
  EXPECT_TRUE(Result.PnpDevices[0].Attached);
  // The guest, rather than an admission gate, reports hardware disappearance.
  completeFile(DriverRequestKind::Read, StatusDeletePending);
  EXPECT_EQ(Result.Requests.back().IOStatus, StatusDeletePending);
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
  auto Remove = begin(DevicePnpRequest::Remove);
  forward(Remove.IRP);
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  finish(Remove.IRP);
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
}

TEST_F(KernelPnpRequest, HeldIoRetainsItsFileAndRouteAcrossStopAndSurprise) {
  attach();
  start();
  completeFile(DriverRequestKind::Create);
  DriverRequest ReadInput;
  ReadInput.Kind = DriverRequestKind::Read;
  ReadInput.OutputSize = 4;
  const auto Read = take(Model->beginRequest(ReadInput));
  const auto File = get(Read.IRP + IRPOriginalFileOffset);
  const auto Stack = get(Read.IRP + IRPStackPointerOffset);
  const auto Buffer = get(Read.IRP + IRPSystemBufferOffset);
  ASSERT_NE(Buffer, 0u);
  put(Buffer, 0x12345678, 4);
  put(Stack + StackControlOffset, StackPendingReturned, 1);
  ok(Model->recordDispatchReturn(Read.IRP, StatusPending));
  const auto ReadIndex = Result.Requests.size() - 1;
  // KernelModel can represent interleaved submissions. The public runner is
  // deliberately serial and cannot use later scenario input as this producer.
  for (auto Minor :
       {DevicePnpRequest::QueryStop, DevicePnpRequest::Stop,
        DevicePnpRequest::Start, DevicePnpRequest::SurpriseRemoval}) {
    completePnp(Minor);
    EXPECT_TRUE(Model->requestPending(Read.IRP));
    EXPECT_EQ(get(Read.IRP + IRPOriginalFileOffset), File);
    EXPECT_EQ(get(Read.IRP + IRPStackPointerOffset), Stack);
    EXPECT_EQ(get(Read.IRP + IRPSystemBufferOffset), Buffer);
    ok(Model->validateGuestAccess(Buffer, 4, false));
    EXPECT_EQ(get(Buffer, 4), 0x12345678u);
    EXPECT_EQ(get(File + FileDeviceOffset), PDO);
    EXPECT_EQ(get(PDO + DeviceAttachedOffset), FDO);
    EXPECT_FALSE(Result.Requests[ReadIndex].Completed);
  }
  status(Read.IRP, StatusDeletePending);
  call("IofCompleteRequest", {Read.IRP, 0});
  ok(Model->finalizeRequest(Read.IRP));
  reject(Model->validateGuestAccess(Buffer, 4, false), "freed");
  EXPECT_EQ(Result.Requests[ReadIndex].IOStatus, StatusDeletePending);
  EXPECT_TRUE(Result.Requests[ReadIndex].Completed);
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
  auto Remove = begin(DevicePnpRequest::Remove);
  forward(Remove.IRP);
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  finish(Remove.IRP);
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelPnpRequest, CleanupAndCloseRemainAvailableAfterQueryRemove) {
  attach();
  start();
  completeFile(DriverRequestKind::Create);
  auto Query = begin(DevicePnpRequest::QueryRemove);
  forward(Query.IRP);
  finish(Query.IRP);
  auto Remove = Model->beginRequest(pnp(DevicePnpRequest::Remove));
  ASSERT_FALSE(bool(Remove));
  reject(Remove.takeError(), "files to close");
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
}

TEST_F(KernelPnpRequest, NamedOpenKeepsItsLifecycleOwnerAfterDetach) {
  attach("\\Device\\PnpNamed");
  start();
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.Device = "\\Device\\PnpNamed";
  auto Open = take(Model->beginRequest(Create));
  status(Open.IRP, StatusSuccess);
  call("IofCompleteRequest", {Open.IRP, 0});
  finish(Open.IRP);
  auto Query = begin(DevicePnpRequest::QueryRemove);
  forward(Query.IRP);
  finish(Query.IRP);
  call("IoDetachDevice", {PDO});
  DriverRequest Read;
  Read.Kind = DriverRequestKind::Read;
  Read.OutputSize = 4;
  auto ReadCall = take(Model->beginRequest(Read));
  EXPECT_EQ(ReadCall.Argument0, FDO);
  EXPECT_EQ(Result.Requests.back().DeviceID, "sensor0");
  status(ReadCall.IRP, StatusNotSupported);
  call("IofCompleteRequest", {ReadCall.IRP, 0});
  finish(ReadCall.IRP, StatusNotSupported);
  auto Rejected = Model->beginRequest(pnp(DevicePnpRequest::Remove));
  ASSERT_FALSE(bool(Rejected));
  reject(Rejected.takeError(), "files to close");
  completeFile(DriverRequestKind::Cleanup);
  completeFile(DriverRequestKind::Close);
}

TEST_F(KernelPnpRequest, NormalRemovalRequiresQueryAndGuestTeardown) {
  attach();
  start();
  auto Rejected = Model->beginRequest(pnp(DevicePnpRequest::Remove));
  ASSERT_FALSE(bool(Rejected));
  reject(Rejected.takeError(), "requires query-remove");
  auto Query = begin(DevicePnpRequest::QueryRemove);
  forward(Query.IRP);
  finish(Query.IRP);
  auto Remove = begin(DevicePnpRequest::Remove);
  forward(Remove.IRP);
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  // The retained route still owns the FDO until dispatch returns.
  ok(Model->validateGuestAccess(FDO + DeviceExtensionOffset, 8, false));
  finish(Remove.IRP);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Removed);
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_TRUE(Result.Devices.empty());
  reject(Model->validateGuestAccess(FDO + DeviceExtensionOffset, 8, false),
         "freed");
}

TEST_F(KernelPnpRequest, CorruptRemoveDeviceInventoryPreservesRouteForRetry) {
  attach();
  EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                    UnknownDeviceType, 0, 0, Scratch}),
            StatusSuccess);
  const uint64_t Upper = get(Scratch);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, FDO}), FDO);
  const auto Remove = begin(DevicePnpRequest::Remove);
  EXPECT_EQ(Remove.Argument0, Upper);

  const auto Stack = get(Remove.IRP + IRPStackPointerOffset);
  std::array<uint8_t, StackCompletionOffset> Prefix;
  ok(Memory->read(Stack, Prefix));
  ok(Memory->write(Stack - StackSize, Prefix));
  call("IofCallDriver", {FDO, Remove.IRP});
  const auto LowerCall = Model->takeGuestCall();
  ASSERT_TRUE(LowerCall);
  ASSERT_EQ(LowerCall->Arguments.size(), 2u);
  EXPECT_EQ(LowerCall->Arguments[0], FDO);
  EXPECT_EQ(forward(Remove.IRP), StatusSuccess);
  const auto Returned =
      take(Model->finishGuestCall(LowerCall->Token, StatusSuccess));
  ASSERT_TRUE(Returned);
  EXPECT_EQ(*Returned, StatusSuccess);
  ASSERT_TRUE(Result.Requests.back().Completed);
  reject(Model->validateGuestAccess(Remove.IRP, 2, false), "freed");

  call("IoDetachDevice", {FDO});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {Upper});
  call("IoDeleteDevice", {FDO});
  ok(Model->recordDispatchReturn(Remove.IRP, StatusSuccess));
  const uint64_t Head = Model->driverObject() + DriverDeviceHead;
  EXPECT_EQ(get(Head), Upper);
  EXPECT_EQ(get(Upper + DeviceNext), FDO);
  const uint64_t SavedNext = get(FDO + DeviceNext);

  for (uint64_t InvalidNext : {Upper, Scratch + 0x800}) {
    // Both a cycle and an unknown node occur after the first route owner in
    // list order. No owner may retire before the entire inventory is checked.
    ok(Model->validateGuestAccess(FDO + DeviceNext, 8, true));
    put(FDO + DeviceNext, InvalidNext);
    reject(Model->finalizeRequest(Remove.IRP), "device list");
    EXPECT_EQ(get(Head), Upper);
    EXPECT_EQ(get(Upper + DeviceNext), FDO);
    EXPECT_EQ(get(FDO + DeviceNext), InvalidNext);
    for (uint64_t Device : {Upper, FDO, PDO})
      ok(Model->validateGuestAccess(Device, 2, false));
    // The completed packet stays retired, but its owning request record and
    // all captured route holds must survive a rejected finalization.
    reject(Model->validateGuestAccess(Remove.IRP, 2, false), "freed");
    reject(Model->recordDispatchReturn(Remove.IRP, StatusSuccess),
           "already recorded");
    put(FDO + DeviceNext, SavedNext);
    ok(Model->snapshot());
    EXPECT_EQ(Result.Devices.size(), 2u);
    EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  }

  ok(Model->finalizeRequest(Remove.IRP));
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  for (uint64_t Device : {Upper, FDO, PDO})
    reject(Model->validateGuestAccess(Device, 2, false), "freed");
}

TEST_F(KernelPnpRequest,
       DetachedButUndeletedGuestDevicePreventsProviderRetirement) {
  attach();
  auto Remove = begin(DevicePnpRequest::Remove);
  forward(Remove.IRP);
  call("IoDetachDevice", {PDO});
  ok(Model->recordDispatchReturn(Remove.IRP, StatusSuccess));
  reject(Model->finalizeRequest(Remove.IRP), "without deleting");
  ok(Model->snapshot());
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_FALSE(Result.Devices.empty());
}

TEST_F(KernelPnpRequest,
       UnattachedProviderCanCompleteDelayedPnPWithoutGuestCallback) {
  ok(Model->finishAddDevice("sensor0", StatusSuccess));
  auto Start = begin(DevicePnpRequest::Start, StatusSuccess, 17);
  EXPECT_EQ(Start.PC, 0u);
  EXPECT_TRUE(Model->requestPending(Start.IRP));
  EXPECT_EQ(Model->nextEventTime(), 17u);
  auto Scheduled = take(Model->nextScheduled(true));
  EXPECT_FALSE(Scheduled);
  EXPECT_FALSE(Model->requestPending(Start.IRP));
  ok(Model->finalizeRequest(Start.IRP));
  EXPECT_EQ(Result.Requests.back().Pnp->BusCompletedAt100ns, 17u);
  EXPECT_FALSE(Result.PnpDevices[0].Attached);
}

TEST_F(KernelPnpRequest, FailedAddDeviceIsAnObservationAndCannotDispatchPnP) {
  ok(Model->finishAddDevice("sensor0", StatusNotSupported));
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, StatusNotSupported);
  auto Request = Model->beginRequest(pnp(DevicePnpRequest::Start));
  ASSERT_FALSE(bool(Request));
  reject(Request.takeError(), "successfully added");
}

TEST_F(KernelPnpRequest, InvalidFinalRemoveLeavesPacketAndTransactionAlive) {
  attach();
  auto Remove = begin(DevicePnpRequest::Remove);
  const auto Before = get(Remove.IRP + IRPStackPointerOffset);
  status(Remove.IRP, StatusNotSupported);
  auto Completion = Model->call("IofCompleteRequest", {Remove.IRP, 0});
  ASSERT_FALSE(bool(Completion));
  reject(Completion.takeError(), "must not fail");
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(Remove.IRP + IRPStackPointerOffset), Before);
  EXPECT_EQ(get(Remove.IRP + IRPStatusOffset, 4), StatusNotSupported);
  ok(Model->validateGuestAccess(Remove.IRP + IRPStatusOffset, 4, false));
}

} // namespace
} // namespace neverd::emulation

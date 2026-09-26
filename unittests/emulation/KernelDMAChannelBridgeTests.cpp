//===- KernelDMAChannelBridgeTests.cpp - Adapter channel integration --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise public WDK table layouts against production model ownership and
/// actual backend RAM, including captured IRPs and explicit transfer ownership.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelExportRegistry.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
class KernelDMAChannelBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Entry = 0x180001000;
  static constexpr uint64_t Logical = 0x20000000, ListPC = Entry + 0x100;
  static constexpr uint64_t Description = Scratch + 0x100;
  static constexpr uint64_t LengthSlot = Scratch + 0x280;
  static constexpr uint64_t ControlPC = Entry + 0x200;
  std::unique_ptr<UnicornBackend> Memory;
  KernelExportRegistry Exports;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t PDO = 0, FDO = 0, Adapter = 0;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  void reject(llvm::Error E, llvm::StringRef Text = {}) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T>
  void reject(llvm::Expected<T> V, llvm::StringRef Text = {}) {
    ASSERT_FALSE(bool(V));
    reject(V.takeError(), Text);
  }
  uint64_t get(uint64_t A, unsigned N = 8) {
    return take(Memory->readInteger(A, N));
  }
  void put(uint64_t A, uint64_t V, unsigned N = 8) {
    ok(Model->validateGuestAccess(A, N, true));
    ok(Memory->writeInteger(A, V, N));
  }
  uint64_t call(const char *N, std::initializer_list<uint64_t> A) {
    return take(Model->call(N, A));
  }
  const KernelExportRegistry::Export &method(unsigned Index,
                                             uint64_t Owner = 0) {
    const auto Table =
        get((Owner ? Owner : Adapter) + dma::AdapterOperationsOffset);
    const auto *Export =
        Exports.lookup(get(Table + dma::OperationFirstOffset + Index * 8));
    EXPECT_NE(Export, nullptr);
    static const KernelExportRegistry::Export Invalid;
    return Export ? *Export : Invalid;
  }
  uint64_t invoke(unsigned I, std::initializer_list<uint64_t> A) {
    return take(Model->call(method(I), A, nullptr));
  }
  void description(uint64_t A, uint32_t Version = 1) {
    put(A, Version, 4);
    put(A + 4, 1, 1);
    put(A + 5, 1, 1);
    put(A + 8, 1, 1);
    put(A + 10, 0, 1);
    put(A + 11, 0, 1);
    put(A + 20, UINT32_MAX, 4);
    put(A + 32, 8192, 4);
  }
  void complete(uint64_t IRP) {
    put(IRP + IRPStatusOffset, StatusSuccess, 4);
    put(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "dma";
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    DriverMemoryResource Resource;
    Resource.ID = "registers";
    Resource.RawStart = 0x300000000;
    Resource.TranslatedStart = 0x400000000;
    Resource.Length = 4096;
    Resource.Registers = {{0, 4, DriverRegisterAccess::ReadWrite, 0}};
    Device.Resources.push_back(Resource);
    Device.Dma = DriverDmaConfig{32, 8192, 3, 1, Logical, 0x100000, true};
    Options.PnpDevices.push_back(Device);
    Result.Configuration = Options;
    ok(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        Entry);
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u, 22u, 27u})
      put(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("dma")).Argument1;
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 64, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    // Acquisition during AddDevice precedes any START resource assignment.
    description(Description);
    Adapter = call("IoGetDmaAdapter", {PDO, Description, Scratch + 8});
    ASSERT_NE(Adapter, 0u);
    ok(Model->finishAddDevice("dma", StatusSuccess));
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "dma";
    Start.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
    auto Packet = take(Model->beginRequest(Start));
    auto Stack = get(Packet.IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Memory->read(Stack, Prefix));
    ok(Memory->write(Stack - StackSize, Prefix));
    EXPECT_EQ(call("IofCallDriver", {PDO, Packet.IRP}), StatusSuccess);
    ok(Model->recordDispatchReturn(Packet.IRP, StatusSuccess));
    ok(Model->finalizeRequest(Packet.IRP));
    DriverRequest Open;
    Open.Kind = DriverRequestKind::Create;
    Open.DeviceID = "dma";
    Open.File = 1;
    Packet = take(Model->beginRequest(Open));
    complete(Packet.IRP);
    ok(Model->recordDispatchReturn(Packet.IRP, StatusSuccess));
    ok(Model->finalizeRequest(Packet.IRP));
    Model->enterExecution(Scratch + 0x8000);
  }
  KernelScheduler::Invocation dpc() {
    call("KeInitializeDpc", {Scratch + 0x400, Entry, Scratch});
    EXPECT_EQ(call("KeInsertQueueDpc", {Scratch + 0x400, 0, 0}), 1u);
    auto Next = take(Model->nextScheduled(false));
    EXPECT_TRUE(Next);
    return Next.value_or(KernelScheduler::Invocation{});
  }
  std::pair<uint64_t, uint64_t> buffer() {
    auto Buffer = call("ExAllocatePoolWithTag", {0, 4096, 0x414d44});
    auto MDL = call("IoAllocateMdl", {Buffer, 32, 0, 0, 0});
    call("MmBuildMdlForNonPagedPool", {MDL});
    return {Buffer, MDL};
  }
  KernelGuestCall getList(uint64_t Buffer, uint64_t MDL) {
    EXPECT_EQ(
        invoke(10, {Adapter, FDO, MDL, Buffer, 32, ListPC, Scratch + 0x900, 1}),
        StatusSuccess);
    auto Guest = Model->takeGuestCall();
    EXPECT_TRUE(Guest);
    if (!Guest)
      return {};
    ok(Model->beginGuestCall(Guest->Token));
    return *Guest;
  }
  struct DataView {
    uint64_t Allocation, Address, MDL;
    uint32_t Length;
  };
  DataView data(uint32_t Length = 8192, uint32_t Offset = 0) {
    const auto Allocation =
        call("ExAllocatePoolWithTag", {0, Length + Offset + 4096, 0x414d44});
    const auto Address = ((Allocation + 4095) & ~uint64_t(4095)) + Offset;
    const auto MDL = call("IoAllocateMdl", {Address, Length, 0, 0, 0});
    call("MmBuildMdlForNonPagedPool", {MDL});
    return {Allocation, Address, MDL, Length};
  }
  KernelModel::Invocation request() {
    DriverRequest Input;
    Input.Kind = DriverRequestKind::DeviceControl;
    Input.DeviceID = "dma";
    Input.File = 1;
    Input.ControlCode = 0x222000;
    Input.OutputSize = 32;
    return take(Model->beginRequest(Input));
  }
  void pending(uint64_t IRP) {
    call("IoMarkIrpPending", {IRP});
    ok(Model->recordDispatchReturn(IRP, StatusPending));
  }
  KernelGuestCall channel(uint32_t Registers = 3,
                          uint64_t Context = Scratch + 0xA00) {
    EXPECT_EQ(invoke(3, {Adapter, FDO, Registers, ControlPC, Context}),
              StatusSuccess);
    auto Guest = Model->takeGuestCall();
    EXPECT_TRUE(Guest);
    if (!Guest)
      return {};
    ok(Model->beginGuestCall(Guest->Token));
    return *Guest;
  }
  void returned(const KernelGuestCall &Guest, uint64_t Action) {
    const auto Return = take(Model->finishGuestCall(Guest.Token, Action));
    ASSERT_TRUE(Return);
    EXPECT_EQ(*Return, StatusSuccess);
  }
  uint64_t transfer(const DataView &Data, uint64_t Token, uint64_t Address,
                    uint32_t Length, bool ToDevice = true) {
    put(LengthSlot, Length, 4);
    return invoke(7, {Adapter, Data.MDL, Token, Address, LengthSlot, ToDevice});
  }
  void flush(const DataView &Data, uint64_t Token, uint64_t Address,
             uint32_t Length, bool ToDevice = true) {
    EXPECT_EQ(invoke(4, {Adapter, Data.MDL, Token, Address, Length, ToDevice}),
              1u);
  }
};

TEST_F(KernelDMAChannelBridge, CurrentIrpWriteIsExactlyOneGuestPointer) {
  ok(Model->validateGuestAccess(FDO + DeviceCurrentIRP, 8, true));
  put(FDO + DeviceCurrentIRP, Scratch);
  EXPECT_EQ(get(FDO + DeviceCurrentIRP), Scratch);
  reject(Model->validateGuestAccess(FDO + DeviceCurrentIRP - 1, 9, true));
  reject(Model->validateGuestAccess(FDO + DeviceCurrentIRP, 9, true));
  reject(Model->validateGuestAccess(FDO + DeviceCurrentIRP + 8, 8, true));
  reject(Model->validateGuestAccess(PDO + DeviceCurrentIRP, 8, true));
  put(FDO + DeviceCurrentIRP, 0);
}

TEST_F(KernelDMAChannelBridge, AdmissionFailuresLeaveNoAcceptedCallback) {
  reject(Model->call(method(3), {Adapter, FDO, 1, ControlPC, 0}, nullptr));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  const auto Parent = dpc();
  EXPECT_EQ(invoke(3, {Adapter, FDO, 4, ControlPC, 0}),
            StatusInsufficientResources);
  reject(Model->call(method(3), {Adapter, FDO, 1, 0, 0}, nullptr));
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  const auto Guest = channel();
  returned(Guest, dma::DeallocateObject);
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge, ActiveRequestAndContextNeverSupplyCurrentIrp) {
  const auto Packet = request();
  pending(Packet.IRP);
  EXPECT_EQ(get(FDO + DeviceCurrentIRP), 0u);
  const auto Parent = dpc();
  const auto Guest = channel(1, Packet.IRP);
  EXPECT_EQ(Guest.Arguments,
            (std::vector<uint64_t>{FDO, 0, Guest.Token.ID, Packet.IRP}));
  returned(Guest, dma::DeallocateObject);
  complete(Packet.IRP);
  ok(Model->finishScheduled(Parent.ID));
  ok(Model->finalizeRequest(Packet.IRP));
}

TEST_F(KernelDMAChannelBridge, BegunCallbackCanCompleteItsCapturedIrp) {
  const auto Packet = request();
  pending(Packet.IRP);
  put(FDO + DeviceCurrentIRP, Packet.IRP);
  const auto Parent = dpc();
  const auto Guest = channel();
  EXPECT_EQ(Guest.Arguments[1], Packet.IRP);
  complete(Packet.IRP);
  EXPECT_TRUE(Result.Requests.back().Completed);
  reject(Model->validateGuestAccess(Packet.IRP, 1, false));
  // Return must use metadata, even though driver-owned CurrentIrp is stale.
  EXPECT_EQ(get(FDO + DeviceCurrentIRP), Packet.IRP);
  returned(Guest, dma::DeallocateObject);
  ok(Model->finishScheduled(Parent.ID));
  ok(Model->finalizeRequest(Packet.IRP));
}

TEST_F(KernelDMAChannelBridge, QueuedSnapshotProtectsIrpOnlyUntilDelivery) {
  const auto Packet = request();
  pending(Packet.IRP);
  const auto Parent = dpc();
  const auto Held = channel();
  returned(Held, dma::DeallocateObjectKeepRegisters);
  put(FDO + DeviceCurrentIRP, Packet.IRP);
  EXPECT_EQ(invoke(3, {Adapter, FDO, 1, ControlPC, Scratch + 0xB00}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  put(FDO + DeviceCurrentIRP, 0);
  put(Packet.IRP + IRPStatusOffset, StatusSuccess, 4);
  put(Packet.IRP + IRPInformationOffset, 0);
  const auto Cursor = get(Packet.IRP + IRPStackPointerOffset);
  const auto Location = get(Packet.IRP + IRPLocationOffset, 1);
  std::array<uint8_t, StackSize> Before, After;
  ok(Memory->read(Cursor, Before));
  reject(Model->call("IofCompleteRequest", {Packet.IRP, 0}));
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(Packet.IRP + IRPStackPointerOffset), Cursor);
  EXPECT_EQ(get(Packet.IRP + IRPLocationOffset, 1), Location);
  ok(Memory->read(Cursor, After));
  EXPECT_EQ(After, Before);
  ok(Model->validateGuestAccess(Packet.IRP + IRPStatusOffset, 4, false));
  invoke(6, {Adapter, Held.Token.ID, 3});
  ok(Model->finishScheduled(Parent.ID));
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::DMAAdapterControl);
  EXPECT_EQ(Next->Arguments[1], Packet.IRP);
  EXPECT_EQ(Next->Arguments[3], Scratch + 0xB00);
  EXPECT_EQ(get(FDO + DeviceCurrentIRP), 0u);
  complete(Packet.IRP);
  reject(Model->validateGuestAccess(Packet.IRP, 1, false));
  EXPECT_FALSE(take(Model->continueScheduled(Next->ID, dma::DeallocateObject)));
  ok(Model->finishScheduled(Next->ID));
  ok(Model->finalizeRequest(Packet.IRP));
}

TEST_F(KernelDMAChannelBridge, UnknownCurrentIrpFailsBeforeReservation) {
  const auto Parent = dpc();
  put(FDO + DeviceCurrentIRP, Scratch);
  reject(Model->call(method(3), {Adapter, FDO, 3, ControlPC, 0}, nullptr));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  EXPECT_FALSE(Model->takeGuestCall());
  put(FDO + DeviceCurrentIRP, 0);
  const auto Guest = channel();
  reject(Model->validateGuestAccess(Guest.Arguments[2], 1, false));
  reject(Model->validateGuestAccess(Guest.Arguments[2], 1, true));
  returned(Guest, dma::DeallocateObject);
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge, ReturnUsesLowDwordAndFailedActionKeepsOwner) {
  const auto Parent = dpc();
  const auto Guest = channel();
  for (uint64_t Invalid : {0u, 4u, 0x10000002u}) {
    reject(Model->finishGuestCall(Guest.Token, Invalid));
    EXPECT_TRUE(Model->hasPendingHardwareWork());
    reject(Model->call(method(0), {Adapter}, nullptr));
  }
  reject(Model->finishGuestCall(Guest.Token, dma::KeepObject));
  returned(Guest, 0xDEADBEEF00000000ULL | dma::DeallocateObjectKeepRegisters);
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  reject(Model->call(method(0), {Adapter}, nullptr));
  invoke(6, {Adapter, Guest.Token.ID, 3});
  const auto Next = channel();
  returned(Next, 0xBAD0C0DE00000000ULL | dma::DeallocateObject);
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge,
       UnflushedTransferBlocksCpuMdlAndRegisterRelease) {
  const auto Data = data();
  const auto Parent = dpc();
  const auto Guest = channel();
  EXPECT_NE(transfer(Data, Guest.Token.ID, Data.Address, 32), 0u);
  reject(Model->finishGuestCall(Guest.Token, dma::DeallocateObject));
  returned(Guest, dma::DeallocateObjectKeepRegisters);
  reject(Model->validateGuestAccess(Data.Address, 1, false));
  reject(Model->validateGuestAccess(Data.Address, 1, true));
  reject(Model->call("IoFreeMdl", {Data.MDL}));
  reject(Model->call("ExFreePool", {Data.Allocation}));
  reject(Model->call(method(6), {Adapter, Guest.Token.ID, 3}, nullptr));
  reject(Model->call(method(4),
                     {Adapter, Data.MDL, Guest.Token.ID, Data.Address, 32, 0},
                     nullptr));
  reject(Model->validateGuestAccess(Data.Address, 1, false));
  flush(Data, Guest.Token.ID, Data.Address, 32);
  ok(Model->validateGuestAccess(Data.Address, 32, true));
  call("IoFreeMdl", {Data.MDL});
  call("ExFreePool", {Data.Allocation});
  invoke(6, {Adapter, Guest.Token.ID, 3});
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge, CacheFlushDoesNotEndTransferOwnership) {
  const auto Data = data();
  const auto Unbuilt = call("IoAllocateMdl", {Data.Address, 32, 0, 0, 0});
  reject(Model->call("KeFlushIoBuffers", {0, 1, 1}));
  reject(Model->call("KeFlushIoBuffers", {Unbuilt, 1, 1}));
  const auto Parent = dpc();
  const auto Guest = channel();
  transfer(Data, Guest.Token.ID, Data.Address, 32);
  for (uint64_t Read : {0, 1})
    for (uint64_t DMA : {0, 1})
      EXPECT_EQ(call("KeFlushIoBuffers", {Data.MDL, Read, DMA}), 0u);
  reject(Model->validateGuestAccess(Data.Address, 1, false));
  reject(Model->call("IoFreeMdl", {Data.MDL}));
  reject(Model->finishGuestCall(Guest.Token, dma::DeallocateObject));
  flush(Data, Guest.Token.ID, Data.Address, 32);
  returned(Guest, dma::DeallocateObject);
  call("IoFreeMdl", {Data.MDL});
  reject(Model->call("KeFlushIoBuffers", {Data.MDL, 1, 1}));
  call("IoFreeMdl", {Unbuilt});
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge, PartialMapsRequireOneAggregateFlush) {
  const auto Data = data(32, 4088);
  const auto Parent = dpc();
  const auto Guest = channel();
  returned(Guest, dma::DeallocateObjectKeepRegisters);
  put(LengthSlot, 0x1122334400000020ULL);
  const auto First = invoke(
      7, {Adapter, Data.MDL, Guest.Token.ID, Data.Address, LengthSlot, 1});
  EXPECT_EQ(get(LengthSlot, 4), 8u);
  EXPECT_EQ(get(LengthSlot + 4, 4), 0x11223344u);
  const auto Second = transfer(Data, Guest.Token.ID, Data.Address + 8, 24);
  EXPECT_EQ(get(LengthSlot, 4), 24u);
  EXPECT_EQ(Second, First + 8);
  reject(Model->call(method(4),
                     {Adapter, Data.MDL, Guest.Token.ID, Data.Address, 8, 1},
                     nullptr));
  reject(Model->call(
      method(4), {Adapter, Data.MDL, Guest.Token.ID, Data.Address + 8, 24, 1},
      nullptr));
  reject(Model->validateGuestAccess(Data.Address, 32, false));
  flush(Data, Guest.Token.ID, Data.Address, 32);
  ok(Model->validateGuestAccess(Data.Address, 32, true));
  // The retained register token accepts another operation, not an old alias.
  const auto Later = transfer(Data, Guest.Token.ID, Data.Address, 8);
  EXPECT_GT(Later, Second + 23);
  flush(Data, Guest.Token.ID, Data.Address, 8);
  invoke(6, {Adapter, Guest.Token.ID, 3});
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge, InvalidMapLeavesLengthAndAdmissionUnchanged) {
  const auto Data = data();
  const auto Parent = dpc();
  const auto Guest = channel();
  put(LengthSlot, 8193, 4);
  reject(Model->call(
      method(7),
      {Adapter, Data.MDL, Guest.Token.ID, Data.Address, LengthSlot, 1},
      nullptr));
  EXPECT_EQ(get(LengthSlot, 4), 8193u);
  put(LengthSlot, 32, 4);
  reject(Model->call(method(7),
                     {Adapter, Data.MDL, Scratch, Data.Address, LengthSlot, 1},
                     nullptr));
  EXPECT_EQ(get(LengthSlot, 4), 32u);
  reject(Model->call(
      method(7), {Adapter, Data.MDL, Guest.Token.ID, Data.Address, Adapter, 1},
      nullptr));
  EXPECT_EQ(get(Adapter, 2), 1u);
  ok(Model->validateGuestAccess(Data.Address, 32, true));
  EXPECT_EQ(transfer(Data, Guest.Token.ID, Data.Address, 32), Logical);
  flush(Data, Guest.Token.ID, Data.Address, 32);
  returned(Guest, dma::DeallocateObject);
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMAChannelBridge,
       RegisterReleasePromotesMixedCallbacksInFifoOrder) {
  const auto Data = data();
  const auto Parent = dpc();
  const auto Held = channel();
  returned(Held, dma::DeallocateObjectKeepRegisters);
  EXPECT_EQ(invoke(10, {Adapter, FDO, Data.MDL, Data.Address, 32, ListPC,
                        Scratch + 0xB00, 1}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(invoke(3, {Adapter, FDO, 2, ControlPC, Scratch + 0xB08}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  invoke(6, {Adapter, Held.Token.ID, 3});
  EXPECT_FALSE(Model->takeGuestCall()); // Release does not reenter the guest.
  ok(Model->finishScheduled(Parent.ID));
  const auto First = take(Model->nextScheduled(false));
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Kind, KernelScheduler::CallbackKind::DMAListControl);
  EXPECT_EQ(First->PC, ListPC);
  EXPECT_EQ(First->IRQL, 2);
  EXPECT_EQ(First->Arguments[3], Scratch + 0xB00);
  invoke(11, {Adapter, First->Arguments[2], 1});
  EXPECT_FALSE(take(Model->continueScheduled(First->ID, UINT64_MAX)));
  ok(Model->finishScheduled(First->ID));
  const auto Second = take(Model->nextScheduled(false));
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->Kind, KernelScheduler::CallbackKind::DMAAdapterControl);
  EXPECT_EQ(Second->PC, ControlPC);
  EXPECT_EQ(Second->IRQL, 2);
  EXPECT_EQ(Second->Arguments[3], Scratch + 0xB08);
  EXPECT_FALSE(
      take(Model->continueScheduled(Second->ID, dma::DeallocateObject)));
  ok(Model->finishScheduled(Second->ID));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelDMAChannelBridge, NestedListCannotConsumeOuterChannelReturn) {
  const auto Data = data();
  const auto Parent = dpc();
  const auto Outer = channel(2);
  const auto Inner = getList(Data.Address, Data.MDL);
  reject(Model->finishGuestCall(Outer.Token, dma::DeallocateObject));
  reject(Model->call(method(3), {Adapter, FDO, 1, ControlPC, 0}, nullptr));
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  invoke(11, {Adapter, Inner.Token.ID, 1});
  returned(Inner, UINT64_MAX); // Void callback still ignores its return bits.
  EXPECT_EQ(transfer(Data, Outer.Token.ID, Data.Address, 32), Logical + 4096);
  flush(Data, Outer.Token.ID, Data.Address, 32);
  returned(Outer, dma::DeallocateObject);
  ok(Model->finishScheduled(Parent.ID));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelDMAChannelBridge, WrongRegisterReleaseDoesNotWakeWaitingChannel) {
  const auto Other = call("IoGetDmaAdapter", {PDO, Description, Scratch + 8});
  const auto Parent = dpc();
  const auto Held = channel(2);
  returned(Held, dma::DeallocateObjectKeepRegisters);
  EXPECT_EQ(invoke(3, {Adapter, FDO, 2, ControlPC, Scratch}), StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  reject(Model->call(method(6), {Adapter, Held.Token.ID, 1}, nullptr));
  reject(Model->call(method(6, Other), {Other, Held.Token.ID, 2}, nullptr));
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  invoke(6, {Adapter, Held.Token.ID, 2});
  reject(Model->call(method(6), {Adapter, Held.Token.ID, 2}, nullptr));
  ok(Model->finishScheduled(Parent.ID));
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::DMAAdapterControl);
  EXPECT_FALSE(take(Model->continueScheduled(Next->ID, dma::DeallocateObject)));
  ok(Model->finishScheduled(Next->ID));
}

TEST_F(KernelDMAChannelBridge,
       NestedListCannotConsumeQueuedOuterChannelReturn) {
  const auto Data = data();
  const auto Parent = dpc();
  const auto Held = channel(3);
  returned(Held, dma::DeallocateObjectKeepRegisters);
  EXPECT_EQ(invoke(3, {Adapter, FDO, 2, ControlPC, Scratch + 0xB08}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  invoke(6, {Adapter, Held.Token.ID, 3});
  ok(Model->finishScheduled(Parent.ID));
  const auto Outer = take(Model->nextScheduled(false));
  ASSERT_TRUE(Outer);
  EXPECT_EQ(Outer->Kind, KernelScheduler::CallbackKind::DMAAdapterControl);
  const auto Inner = getList(Data.Address, Data.MDL);
  reject(Model->continueScheduled(Outer->ID, dma::DeallocateObject));
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  invoke(11, {Adapter, Inner.Token.ID, 1});
  returned(Inner, UINT64_MAX);
  EXPECT_EQ(transfer(Data, Outer->Arguments[2], Data.Address, 32),
            Logical + 4096);
  flush(Data, Outer->Arguments[2], Data.Address, 32);
  EXPECT_FALSE(
      take(Model->continueScheduled(Outer->ID, dma::DeallocateObject)));
  ok(Model->finishScheduled(Outer->ID));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelDMAChannelBridge, DirectInputWriteDirectionFailsBeforeMapEffects) {
  DriverRequest Input;
  Input.Kind = DriverRequestKind::DeviceControl;
  Input.DeviceID = "dma";
  Input.File = 1;
  Input.ControlCode = 0x222001; // METHOD_IN_DIRECT has no DMA write lock.
  Input.OutputSize = 32;
  const auto Packet = take(Model->beginRequest(Input));
  pending(Packet.IRP);
  const auto MDL = get(Packet.IRP + IRPMdlOffset);
  const auto Virtual =
      get(MDL + MDLStartVAOffset) + get(MDL + MDLByteOffsetOffset, 4);
  call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  const auto Parent = dpc();
  const auto Guest = channel();
  put(LengthSlot, 32, 4);
  reject(Model->call(method(7),
                     {Adapter, MDL, Guest.Token.ID, Virtual, LengthSlot, 0},
                     nullptr));
  EXPECT_EQ(get(LengthSlot, 4), 32u);
  EXPECT_NE(invoke(7, {Adapter, MDL, Guest.Token.ID, Virtual, LengthSlot, 1}),
            0u);
  EXPECT_EQ(invoke(4, {Adapter, MDL, Guest.Token.ID, Virtual, 32, 1}), 1u);
  returned(Guest, dma::DeallocateObject);
  complete(Packet.IRP);
  ok(Model->finishScheduled(Parent.ID));
  ok(Model->finalizeRequest(Packet.IRP));
}

} // namespace
} // namespace neverd::emulation

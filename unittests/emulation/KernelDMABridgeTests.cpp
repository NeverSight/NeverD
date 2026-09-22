//===- KernelDMABridgeTests.cpp - Real adapter and scheduler integration --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise public WDK table layouts against production model ownership and
/// actual backend RAM, including reentrant and resource-waiting callbacks.
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
class KernelDMABridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Entry = 0x180001000;
  static constexpr uint64_t Logical = 0x20000000, ListPC = Entry + 0x100;
  static constexpr uint64_t Description = Scratch + 0x100;
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
  void reject(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T> void reject(llvm::Expected<T> V, llvm::StringRef Text) {
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
    put(A + 32, 4096, 4);
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
    Device.Dma = DriverDmaConfig{32, 4096, 1, 1, Logical, 0x100000, true};
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
};

TEST_F(KernelDMABridge, HistoricalPrefixAndReturnedTableAreExact) {
  EXPECT_EQ(get(Adapter, 2), 1u);
  EXPECT_EQ(get(Adapter + 2, 2), 16u);
  EXPECT_EQ(get(get(Adapter + 8), 4), 104u);
  EXPECT_EQ(get(Scratch + 8, 4), 1u);
  EXPECT_EQ(invoke(8, {Adapter}), 1u);
  const auto Old = Scratch + 0x10000 - 40;
  description(Old);
  EXPECT_NE(call("IoGetDmaAdapter", {PDO, Old, Scratch + 8}), 0u);
  const auto Probe = Scratch + 0x10000 - 4;
  put(Probe, 2, 4);
  EXPECT_EQ(call("IoGetDmaAdapter", {PDO, Probe, Scratch + 8}), 0u);
}

TEST_F(KernelDMABridge, BoundMethodsDoNotAliasAnotherAdapterOrKernelNamespace) {
  const auto Second = call("IoGetDmaAdapter", {PDO, Description, Scratch + 8});
  reject(Model->call(method(8), {Second}, nullptr), "binding");
  const auto Retired = method(8);
  invoke(0, {Adapter});
  reject(Model->call(Retired, {Adapter}, nullptr), "binding");
  reject(Model->validateGuestAccess(Adapter, 2, false), "retired");
  reject(Exports.resolve("GetDmaAlignment"), "unspecified");
}

TEST_F(KernelDMABridge, CommonDeviceWritePublishesAfterIrpCompletion) {
  const auto Buffer = invoke(1, {Adapter, 32, Scratch + 0x200, 1});
  ASSERT_NE(Buffer, 0u);
  EXPECT_EQ(get(Scratch + 0x200), Logical);
  DriverRequest Input;
  Input.Kind = DriverRequestKind::DeviceControl;
  Input.DeviceID = "dma";
  Input.File = 1;
  Input.ControlCode = 0x222000;
  Input.DmaEvents = {
      {7, "dma", Logical, DriverDmaDirection::WriteMemory, 4, {1, 2, 3, 4}}};
  const auto Packet = take(Model->beginRequest(Input, 29));
  complete(Packet.IRP);
  ok(Model->recordDispatchReturn(Packet.IRP, StatusSuccess));
  ok(Model->finalizeRequest(Packet.IRP));
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  ASSERT_EQ(Result.DmaTransfers.size(), 1u);
  EXPECT_EQ(Result.DmaTransfers[0].SourceRequestIndex, 29u);
  EXPECT_EQ(Result.DmaTransfers[0].CompletedAt100ns, 7u);
  EXPECT_EQ(get(Buffer, 4), 0x04030201u);
  invoke(2, {Adapter, 32, Logical, Buffer, 0});
  reject(Model->validateGuestAccess(Buffer, 1, false), "freed");
}

TEST_F(KernelDMABridge,
       CommonFreeChecksExactExtentAndFutureAddressIsNotReused) {
  const auto First = invoke(1, {Adapter, 32, Scratch + 0x200, 0});
  reject(Model->call(method(2), {Adapter, 31, Logical, First, 0}, nullptr),
         "exact");
  reject(Model->call(method(0), {Adapter}, nullptr), "owns");
  invoke(2, {Adapter, 32, Logical, First, 1});
  const auto Next = invoke(1, {Adapter, 32, Scratch + 0x200, 1});
  EXPECT_NE(Next, First);
  EXPECT_EQ(get(Scratch + 0x200), Logical + 4096);
}

TEST_F(KernelDMABridge,
       ImmediateCallbackOwnsMdlAndVoidReturnDoesNotChangeGetStatus) {
  const auto [Buffer, MDL] = buffer();
  const auto Parent = dpc();
  const auto Guest = getList(Buffer, MDL);
  EXPECT_EQ(Guest.Token.Owner, GuestCallOwner::DMA);
  EXPECT_EQ(Guest.Arguments,
            (std::vector<uint64_t>{FDO, 0, Guest.Token.ID, Scratch + 0x900}));
  reject(Model->call("IoFreeMdl", {MDL}), "DMA");
  reject(Model->call("ExFreePool", {Buffer}), "pinned");
  reject(Model->validateGuestAccess(Buffer, 1, false), "DMA");
  invoke(11, {Adapter, Guest.Token.ID, 1});
  ok(Model->validateGuestAccess(Buffer, 1, true));
  call("IoFreeMdl", {MDL});
  call("ExFreePool", {Buffer});
  const auto Return = take(Model->finishGuestCall(Guest.Token, 0xfeedbeef));
  ASSERT_TRUE(Return);
  EXPECT_EQ(*Return, StatusSuccess);
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMABridge,
       ExhaustedMapRegistersReserveAndLaterDispatchSecondCallback) {
  const auto A = buffer(), B = buffer();
  const auto Parent = dpc();
  const auto First = getList(A.first, A.second);
  EXPECT_TRUE(take(Model->finishGuestCall(First.Token, 99)));
  EXPECT_EQ(invoke(10, {Adapter, FDO, B.second, B.first, 32, ListPC,
                        Scratch + 0x908, 1}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  reject(Model->call("IoFreeMdl", {B.second}), "DMA");
  invoke(11, {Adapter, First.Token.ID, 1});
  ok(Model->finishScheduled(Parent.ID));
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::DMAListControl);
  EXPECT_EQ(Next->Arguments[1], 0u);
  EXPECT_EQ(Next->Arguments[3], Scratch + 0x908);
  invoke(11, {Adapter, Next->Arguments[2], 1});
  EXPECT_FALSE(take(Model->continueScheduled(Next->ID, UINT64_MAX)));
  ok(Model->finishScheduled(Next->ID));
  call("IoFreeMdl", {A.second});
  call("IoFreeMdl", {B.second});
  call("ExFreePool", {A.first});
  call("ExFreePool", {B.first});
}

TEST_F(KernelDMABridge, AdapterCanRetireInsideLastCallbackAfterPut) {
  const auto [Buffer, MDL] = buffer();
  const auto Parent = dpc();
  const auto Guest = getList(Buffer, MDL);
  const auto Release = method(0);
  invoke(11, {Adapter, Guest.Token.ID, 1});
  EXPECT_EQ(take(Model->call(Release, {Adapter}, nullptr)), 0u);
  EXPECT_TRUE(take(Model->finishGuestCall(Guest.Token, UINT64_MAX)));
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMABridge, MethodIrqlAndWrongDirectionFailBeforeOwnershipChanges) {
  const auto [Buffer, MDL] = buffer();
  reject(Model->call(method(10), {Adapter, FDO, MDL, Buffer, 32, ListPC, 0, 1},
                     nullptr),
         "IRQL");
  const auto Parent = dpc();
  const auto Guest = getList(Buffer, MDL);
  reject(Model->call(method(11), {Adapter, Guest.Token.ID, 0}, nullptr),
         "direction");
  reject(Model->validateGuestAccess(Buffer, 1, false), "DMA");
  invoke(11, {Adapter, Guest.Token.ID, 1});
  EXPECT_TRUE(take(Model->finishGuestCall(Guest.Token, 0)));
  ok(Model->finishScheduled(Parent.ID));
}
TEST_F(KernelDMABridge, NestedInlineReturnRejectsOuterWithoutConsumingIt) {
  const auto [Buffer, MDL] = buffer();
  const auto Parent = dpc();
  const auto Outer = getList(Buffer, MDL);
  invoke(11, {Adapter, Outer.Token.ID, 1});
  const auto Inner = getList(Buffer, MDL);
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  reject(Model->finishGuestCall(Outer.Token, 0), "DMA");
  invoke(11, {Adapter, Inner.Token.ID, 1});
  EXPECT_TRUE(take(Model->finishGuestCall(Inner.Token, UINT64_MAX)));
  EXPECT_TRUE(take(Model->finishGuestCall(Outer.Token, UINT64_MAX)));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  ok(Model->finishScheduled(Parent.ID));
}

TEST_F(KernelDMABridge,
       QueuedCallbackCanNestInlineWithoutReplacingScheduledOwner) {
  const auto [Buffer, MDL] = buffer();
  const auto Parent = dpc();
  const auto First = getList(Buffer, MDL);
  EXPECT_TRUE(take(Model->finishGuestCall(First.Token, 0)));
  EXPECT_EQ(invoke(10, {Adapter, FDO, MDL, Buffer, 32, ListPC, 0, 1}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeGuestCall());
  invoke(11, {Adapter, First.Token.ID, 1});
  ok(Model->finishScheduled(Parent.ID));
  auto Queued = take(Model->nextScheduled(false));
  ASSERT_TRUE(Queued);
  invoke(11, {Adapter, Queued->Arguments[2], 1});
  const auto Inner = getList(Buffer, MDL);
  invoke(11, {Adapter, Inner.Token.ID, 1});
  EXPECT_TRUE(take(Model->finishGuestCall(Inner.Token, UINT64_MAX)));
  EXPECT_FALSE(take(Model->continueScheduled(Queued->ID, UINT64_MAX)));
  ok(Model->finishScheduled(Queued->ID));
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelDMABridge,
       RevokingCpuMappingKeepsDmaPinsAndCompletionReadsSameBacking) {
  DriverRequest Input;
  Input.Kind = DriverRequestKind::DeviceControl;
  Input.DeviceID = "dma";
  Input.File = 1;
  Input.ControlCode = 0x222002;
  Input.OutputSize = 16;
  const auto Request = take(Model->beginRequest(Input));
  const auto MDL = get(Request.IRP + IRPMdlOffset);
  const auto Virtual =
      get(MDL + MDLStartVAOffset) + get(MDL + MDLByteOffsetOffset, 4);
  const auto Data =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  ASSERT_NE(Data, 0u);
  call("IoMarkIrpPending", {Request.IRP});
  ok(Model->recordDispatchReturn(Request.IRP, StatusPending));
  const auto Parent = dpc();
  EXPECT_EQ(invoke(10, {Adapter, FDO, MDL, Virtual, 16, ListPC, 0, 0}),
            StatusSuccess);
  auto Guest = Model->takeGuestCall();
  ASSERT_TRUE(Guest);
  ok(Model->beginGuestCall(Guest->Token));
  EXPECT_TRUE(take(Model->finishGuestCall(Guest->Token, 0)));
  call("MmUnmapLockedPages", {Data, MDL});
  ok(Memory->validateBacking(Data, 16));
  const std::vector<uint8_t> Bytes(16, 0x5a);
  ok(Memory->writeBacking(Data, Bytes));
  put(Request.IRP + IRPStatusOffset, StatusSuccess, 4);
  put(Request.IRP + IRPInformationOffset, 16);
  invoke(11, {Adapter, Guest->Token.ID, 0});
  call("IofCompleteRequest", {Request.IRP, 0});
  EXPECT_EQ(Result.Requests.back().Output, Bytes);
  ok(Model->finishScheduled(Parent.ID));
  ok(Model->finalizeRequest(Request.IRP));
}

} // namespace
} // namespace neverd::emulation

//===- KernelDMAChannelTests.cpp - Reservations and mapped operations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Adapter domains, allocation pins and callback holds are independent; bus
/// transactions affect the same RAM only after complete logical validation.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelDMA.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Base = 0x100000;
constexpr uint64_t Page = DriverDmaPageSize;
constexpr uint64_t Logical = 0x80000000;
constexpr uint64_t PDO = 0x1000;
constexpr uint64_t AdapterID = 0x2000;

llvm::Error testError(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

class DMARAM final : public GuestMemory {
public:
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(32 * Page);
  std::optional<uint64_t> Hole;
  unsigned DeviceReads = 0, DeviceWrites = 0;
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return testError("test RAM is already mapped");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return llvm::Error::success();
  }
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override {
    if (Address < Base || Address - Base > Bytes.size() ||
        Size > Bytes.size() - (Address - Base) ||
        (Hole && Address <= *Hole && Size > *Hole - Address))
      return testError("test backing hole");
    return llvm::Error::success();
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Out) override {
    if (auto E = validateBacking(Address, Out.size()))
      return E;
    std::copy_n(Bytes.begin() + (Address - Base), Out.size(), Out.begin());
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> In) override {
    if (auto E = validateBacking(Address, In.size()))
      return E;
    std::copy(In.begin(), In.end(), Bytes.begin() + (Address - Base));
    return llvm::Error::success();
  }
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Out) override {
    ++DeviceReads;
    return read(Address, Out);
  }
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> In) override {
    ++DeviceWrites;
    return write(Address, In);
  }
};

class DriverKernelDMAChannel : public ::testing::Test {
protected:
  DMARAM Memory;
  KernelPhysicalMemory Physical{Memory};
  KernelResources Resources{[](uint64_t) { return llvm::Error::success(); }};
  DriverResult Result;
  KernelDMA Model{Physical, Resources, Result};
  uint64_t NextMap = 0x400000;

  static void good(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  static void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <typename T> static T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <typename T>
  static void bad(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    bad(Value.takeError(), Text);
  }

  void configure(uint64_t Device = PDO, llvm::StringRef ID = "bus",
                 uint32_t Registers = 3) {
    DriverPnpDevice Config;
    Config.ID = ID.str();
    Config.Bus = DriverBusKind::RegisterBank;
    Config.InitialDevicePower = DevicePowerState::D0;
    Config.Dma =
        DriverDmaConfig{32, 4 * Page, Registers, 1, Logical, 32 * Page, true};
    good(Resources.configure(Device, Config));
  }
  void start(uint64_t Device = PDO) {
    good(Resources.beginStart(Device));
    good(Resources.completeLowerStart(Device, 0));
    good(Resources.finishPnp(Device, DevicePnpRequest::Start, 0));
  }
  void create(uint64_t ID = AdapterID, uint64_t Device = PDO) {
    const auto *Resource = Resources.find(Device);
    ASSERT_NE(Resource, nullptr);
    ASSERT_TRUE(Resource->Dma);
    const auto &Dma = *Resource->Dma;
    good(Model.createAdapter({ID, ID + 0x100, Device, UINT32_MAX,
                              Dma.MaximumLength, Dma.MapRegisters,
                              Dma.Alignment, true, true}));
  }
  void owner(uint64_t ID, uint64_t Address, uint64_t Size) {
    good(Physical.registerRegion(ID, Address, Size));
  }
  KernelDMA::Mapping
  plan(uint64_t Owner, uint32_t Length, bool Common = false,
       uint64_t Offset = 0, uint64_t Adapter = AdapterID,
       DriverDmaDirection Direction = DriverDmaDirection::ReadMemory) {
    auto Prepared = take(
        Model.planMapping(Adapter, Owner, Offset, Length, Common, Direction));
    if (!Prepared) {
      ADD_FAILURE() << "test mapping unexpectedly ran out of resources";
      return {};
    }
    auto Map = *Prepared;
    Map.Object = Common ? Map.Backing : NextMap;
    NextMap += Page;
    Map.StorageSize = Common
                          ? ((Length + Page - 1) / Page) * Page
                          : dma::ScatterGatherHeaderSize +
                                Map.Registers * dma::ScatterGatherElementSize;
    if (!Common) {
      Map.MDL = Map.Object + 0x800;
      Map.MDLSize = 0x40;
      Map.Device = 0x3000;
      Map.DeviceSize = 0x100;
      Map.Routine = 0x180001000;
      Map.Context = 0xDEADBEEF;
      Map.SchedulerID = Map.Object;
    }
    return Map;
  }
  KernelDMA::Mapping publish(KernelDMA::Mapping Map) {
    good(Model.publishMapping(Map));
    const auto *Stored = Model.mapping(Map.Object);
    if (!Stored)
      return {};
    return *Stored;
  }
  void release(uint64_t Object) {
    good(Model.releaseMapping(take(Model.planRelease(Object))));
  }
  static DriverDmaEvent event(uint64_t Address, uint32_t Length,
                              uint64_t Delay = 0,
                              std::vector<uint8_t> Data = {}) {
    DriverDmaEvent Input;
    Input.DeviceID = "bus";
    Input.LogicalAddress = Address;
    Input.Length = Length;
    Input.After100ns = Delay;
    Input.Direction = Data.empty() ? DriverDmaDirection::ReadMemory
                                   : DriverDmaDirection::WriteMemory;
    Input.Data = std::move(Data);
    return Input;
  }
  KernelDMA::Channel allocate(uint64_t Token = 0x500000, uint32_t Count = 2,
                              uint64_t Device = 0x3000,
                              uint64_t Adapter = AdapterID, uint64_t IRP = 0) {
    KernelDMA::ChannelRequest Request;
    Request.Object = Token;
    Request.Adapter = Adapter;
    Request.Device = Device;
    Request.DeviceSize = 0x100;
    Request.Routine = 0x180002000;
    Request.Context = 0x12345678;
    Request.CurrentIRP = IRP;
    Request.IRPSize = IRP ? 0xD0 : 0;
    Request.SchedulerID = Token;
    Request.Registers = Count;
    auto Plan = take(Model.planChannel(Request));
    if (!Plan) {
      ADD_FAILURE() << "unexpected channel quota failure";
      return {};
    }
    good(Model.publishChannel(*Plan));
    return *Plan;
  }
  KernelDMA::TransferRequest transfer(uint64_t Token = 0x500000,
                                      uint64_t Offset = Page - 8,
                                      uint32_t Length = 32) {
    KernelDMA::TransferRequest Request;
    Request.Adapter = AdapterID;
    Request.Object = Token;
    Request.Owner = 1;
    Request.Offset = Offset;
    Request.MDL = 0x600000;
    Request.MDLSize = 0x50;
    Request.CurrentVA = 0x70000000 + Offset;
    Request.Length = Length;
    return Request;
  }
  KernelDMA::TransferPlan map(KernelDMA::TransferRequest Request) {
    auto Plan = take(Model.planTransfer(Request));
    good(Model.commitTransfer(Plan));
    return Plan;
  }
  void flush(uint64_t Token = 0x500000) {
    const auto *Map = Model.mapping(Token);
    ASSERT_NE(Map, nullptr);
    const auto *Channel = Model.channel(Token);
    ASSERT_NE(Channel, nullptr);
    good(Model.flush(
        take(Model.planFlush(Map->Adapter, Token, Map->MDL, Channel->InitialVA,
                             Map->Length, Map->Direction))));
  }
  void returned(uint64_t Token, uint32_t Action) {
    good(Model.finishChannelReturn(
        take(Model.planChannelReturn(Token, Action))));
  }
  void SetUp() override {
    configure();
    create();
    owner(1, Base, 4 * Page);
  }
};

TEST_F(DriverKernelDMAChannel, SharedQuotaPromotesChannelBeforeLaterSG) {
  const auto Common = publish(plan(1, Page, true));
  const auto SG = publish(plan(1, Page));
  const auto Channel = allocate();
  const auto Later = publish(plan(1, Page));
  EXPECT_FALSE(Channel.Live);
  EXPECT_FALSE(Later.Live);
  good(Model.beginCallback(SG.Object));
  const auto Release = take(Model.planRelease(SG.Object));
  ASSERT_EQ(Release.Ready.size(), 1u);
  EXPECT_EQ(Release.Ready[0].Kind, KernelDMA::CallbackKind::AdapterControl);
  EXPECT_EQ(Release.Ready[0].Object, Channel.Request.Object);
  EXPECT_EQ(Release.Ready[0].SchedulerID, Channel.Request.SchedulerID);
  good(Model.releaseMapping(Release));
  EXPECT_TRUE(Model.channel(Channel.Request.Object)->Live);
  EXPECT_FALSE(Model.mapping(Later.Object)->Live);
  good(Model.beginCallback(Channel.Request.Object));
  const auto Return = take(
      Model.planChannelReturn(Channel.Request.Object, dma::DeallocateObject));
  ASSERT_EQ(Return.Ready.size(), 1u);
  EXPECT_EQ(Return.Ready[0].Kind, KernelDMA::CallbackKind::ScatterGather);
  EXPECT_EQ(Return.Ready[0].Object, Later.Object);
  auto Changed = Return;
  Changed.Ready.clear();
  bad(Model.finishChannelReturn(Changed), "changed");
  EXPECT_FALSE(Model.mapping(Later.Object)->Live);
  good(Model.finishChannelReturn(Return));
  EXPECT_TRUE(Model.mapping(Later.Object)->Live);
  good(Model.finishCallback(SG.Object));
  good(Model.beginCallback(Later.Object));
  release(Later.Object);
  good(Model.finishCallback(Later.Object));
  release(Common.Object);
}

TEST_F(DriverKernelDMAChannel,
       WaitingChannelNeverLetsSmallerRequestBypassFIFO) {
  const auto Common = publish(plan(1, 2 * Page, true));
  const auto Channel = allocate(0x500000, 2);
  const auto Later = allocate(0x500100, 1, 0x3200);
  EXPECT_FALSE(Channel.Live);
  EXPECT_FALSE(Later.Live);
  const auto Release = take(Model.planRelease(Common.Object));
  ASSERT_EQ(Release.Ready.size(), 2u);
  EXPECT_EQ(Release.Ready[0].Object, Channel.Request.Object);
  EXPECT_EQ(Release.Ready[1].Object, Later.Request.Object);
  good(Model.releaseMapping(Release));
  EXPECT_TRUE(Model.channel(Channel.Request.Object)->Live);
  EXPECT_TRUE(Model.channel(Later.Request.Object)->Live);
}

TEST_F(DriverKernelDMAChannel, CurrentIrpIsHeldUntilEntryAndDeviceUntilFree) {
  const auto C = allocate(0x500000, 2, 0x3000, AdapterID, 0x800000);
  auto Info = Model.callbackInfo(C.Request.Object);
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->CurrentIRP, 0x800000u);
  EXPECT_EQ(Info->Context, C.Request.Context);
  bad(Model.canReleaseRange(Info->CurrentIRP, Info->IRPSize), "IRP storage");
  bad(Model.canReleaseRange(Info->Device, Info->DeviceSize), "device storage");
  good(Model.canReleaseRange(C.Request.Object, dma::ChannelTokenSize));
  bad(Model.validateGuestAccess(C.Request.Object, 1, false), "opaque");
  good(Model.beginCallback(C.Request.Object));
  good(Model.canReleaseRange(Info->CurrentIRP, Info->IRPSize));
  returned(C.Request.Object, dma::DeallocateObjectKeepRegisters);
  EXPECT_FALSE(Model.callbackInfo(C.Request.Object));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  bad(Model.putAdapter(AdapterID), "channel");
  bad(Model.canReleaseRange(Info->Device, Info->DeviceSize), "device storage");
  good(Model.freeRegisters(
      take(Model.planFreeRegisters(AdapterID, C.Request.Object, 2))));
  good(Model.canReleaseRange(Info->Device, Info->DeviceSize));
  bad(Model.validateGuestAccess(C.Request.Object, 1, false), "retired");
  good(Model.putAdapter(AdapterID));
}

TEST_F(DriverKernelDMAChannel,
       SingleDeviceCallbackAndAnyRecursiveAllocateReject) {
  const auto C = allocate();
  auto R = C.Request;
  R.Object += 0x100;
  bad(Model.planChannel(R), "outstanding");
  R.Device += 0x200;
  good(Model.beginCallback(C.Request.Object));
  bad(Model.planChannel(R), "inside AdapterControl");
  returned(C.Request.Object, dma::DeallocateObjectKeepRegisters);
  EXPECT_TRUE(take(Model.planChannel(R)).has_value());
  R.Device = C.Request.Device;
  EXPECT_TRUE(take(Model.planChannel(R)).has_value());
}

TEST_F(DriverKernelDMAChannel,
       OversizedAllocationReturnsShortageWithoutCallback) {
  const auto C = allocate();
  auto R = C.Request;
  R.Object += 0x100;
  R.Device += 0x200;
  R.Registers = 4;
  EXPECT_FALSE(take(Model.planChannel(R)));
  EXPECT_FALSE(Model.callbackInfo(R.Object));
  EXPECT_EQ(Model.channel(R.Object), nullptr);
}

TEST_F(DriverKernelDMAChannel, SGFragmentsGrowOnePinAndFlushOneAggregate) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  const auto First = map(transfer());
  EXPECT_EQ(First.Length, 8u);
  EXPECT_EQ(First.Logical, Logical + Page - 8);
  const uint64_t Pin = Model.mapping(C.Request.Object)->Pin;
  auto Next = transfer();
  Next.Offset += 8;
  Next.CurrentVA += 8;
  Next.Length -= 8;
  const auto Second = map(Next);
  EXPECT_EQ(Second.Length, 24u);
  EXPECT_EQ(Second.Logical, Logical + Page);
  EXPECT_EQ(Model.mapping(C.Request.Object)->Pin, Pin);
  EXPECT_EQ(Model.mapping(C.Request.Object)->Length, 32u);
  EXPECT_EQ(Model.mapping(C.Request.Object)->Registers, 2u);
  EXPECT_TRUE(Model.mapping(C.Request.Object)->Channel);
  EXPECT_EQ(Model.mapping(C.Request.Object)->StorageSize, 0u);
  bad(Model.scatterGatherBytes(*Model.mapping(C.Request.Object)),
      "packet view");
  bad(Model.planRelease(C.Request.Object), "FlushAdapterBuffers");
  bad(Model.planFlush(AdapterID, C.Request.Object, Next.MDL, Next.CurrentVA, 24,
                      Next.Direction),
      "complete original");
  bad(Model.validateGuestAccess(Base + Page - 8, 32, false),
      "FlushAdapterBuffers");
  bad(Physical.retire(1), "pinned");
  flush();
  good(Model.validateGuestAccess(Base + Page - 8, 32, true));
  std::array<uint8_t, 1> Byte{};
  bad(Physical.read(Pin, 0, Byte), "pin");
  returned(C.Request.Object, dma::DeallocateObjectKeepRegisters);
  const auto Reused = map(transfer());
  EXPECT_EQ(Reused.Logical, Logical + 3 * Page - 8);
  EXPECT_NE(Model.mapping(C.Request.Object)->Pin, Pin);
}

TEST_F(DriverKernelDMAChannel, AggregateBusTransactionCrossesMapFragments) {
  start();
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  auto R = transfer();
  R.Direction = DriverDmaDirection::WriteMemory;
  const auto First = map(R);
  R.Offset += First.Length;
  R.CurrentVA += First.Length;
  R.Length -= First.Length;
  map(R);
  const std::vector<uint8_t> Bytes(32, 0xAB);
  good(Model.arm({event(First.Logical, 32, 0, Bytes)}, 0, 0));
  good(Model.processEvents(0));
  ASSERT_EQ(Result.DmaTransfers.size(), 1u);
  EXPECT_EQ(Result.DmaTransfers[0].CompletedAt100ns, 0u);
  EXPECT_TRUE(
      std::equal(Bytes.begin(), Bytes.end(), Memory.Bytes.begin() + Page - 8));
  EXPECT_EQ(Memory.DeviceWrites, 1u);
}

TEST_F(DriverKernelDMAChannel,
       AggregatePreflightRejectsHoleBeforeAnyPrefixWrite) {
  start();
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  auto R = transfer();
  R.Direction = DriverDmaDirection::WriteMemory;
  const auto First = map(R);
  R.Offset += First.Length;
  R.CurrentVA += First.Length;
  R.Length -= First.Length;
  map(R);
  Memory.Hole = Base + Page;
  const auto Before = Memory.Bytes;
  good(Model.arm({event(First.Logical, 32, 0, std::vector<uint8_t>(32, 0xAB))},
                 0, 0));
  bad(Model.processEvents(0), "hole");
  EXPECT_EQ(Memory.Bytes, Before);
  EXPECT_EQ(Memory.DeviceWrites, 0u);
}

TEST_F(DriverKernelDMAChannel,
       FailedAppendNeverGrowsPinOrConsumesLogicalSpace) {
  const auto C = allocate(0x500000, 1);
  good(Model.beginCallback(C.Request.Object));
  const auto First = map(transfer());
  const uint64_t Pin = Model.mapping(C.Request.Object)->Pin;
  auto Next = transfer();
  Next.Offset += First.Length;
  Next.CurrentVA += First.Length;
  Next.Length -= First.Length;
  bad(Model.planTransfer(Next), "reserved registers");
  EXPECT_EQ(Model.mapping(C.Request.Object)->Length, 8u);
  std::array<uint8_t, 1> Byte{};
  bad(Physical.read(Pin, 8, Byte), "pinned view");
  flush();
  const auto Again = map(transfer());
  EXPECT_EQ(Again.Logical, First.Logical + Page);
}

TEST_F(DriverKernelDMAChannel,
       AppendRequiresSameMdlDirectionAndContiguousIndices) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  map(transfer());
  auto Next = transfer(0x500000, Page, 24);
  for (unsigned Case = 0; Case < 4; ++Case) {
    auto Bad = Next;
    if (Case == 0)
      ++Bad.MDL;
    if (Case == 1)
      Bad.Direction = DriverDmaDirection::WriteMemory;
    if (Case == 2)
      ++Bad.CurrentVA;
    if (Case == 3)
      ++Bad.Offset;
    bad(Model.planTransfer(Bad), "one MDL");
  }
  EXPECT_EQ(Model.mapping(C.Request.Object)->Length, 8u);
  map(Next);
  EXPECT_EQ(Model.mapping(C.Request.Object)->Length, 32u);
}

TEST_F(DriverKernelDMAChannel,
       InterleavedSGCannotOverlapReservedOperationAperture) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  const auto First = map(transfer());
  const auto SG = publish(plan(1, 4));
  EXPECT_EQ(SG.Logical, Logical + 2 * Page);
  const auto Second = map(transfer(C.Request.Object, Page, 24));
  EXPECT_LT(Second.Logical + Second.Length, SG.Logical);
  flush();
  const auto Again = map(transfer());
  EXPECT_EQ(Again.Logical, Logical + 4 * Page - 8);
  EXPECT_GT(Again.Logical, SG.Logical + SG.Length);
  EXPECT_EQ(First.NextLogical, SG.Logical);
}

TEST_F(DriverKernelDMAChannel,
       NonSGMapsCompleteLengthOrLeavesEverythingUnchanged) {
  good(Model.putAdapter(AdapterID));
  const auto &Dma = *Resources.find(PDO)->Dma;
  good(Model.createAdapter({AdapterID + 0x400, AdapterID + 0x500, PDO,
                            UINT32_MAX, Dma.MaximumLength, Dma.MapRegisters,
                            Dma.Alignment, false, true}));
  const uint64_t ID = AdapterID + 0x400;
  const auto C = allocate(0x500000, 1, 0x3000, ID);
  good(Model.beginCallback(C.Request.Object));
  auto R = transfer();
  R.Adapter = ID;
  bad(Model.planTransfer(R), "reserved registers");
  EXPECT_EQ(Model.mapping(C.Request.Object), nullptr);
  good(Physical.canRetire(1));
  R.Offset = 0;
  R.CurrentVA = 0x70000000;
  const auto Full = map(R);
  EXPECT_EQ(Full.Length, R.Length);
  EXPECT_EQ(Full.Logical, Logical);
  R.Offset += R.Length;
  R.CurrentVA += R.Length;
  bad(Model.planTransfer(R), "flush before another map");
}

TEST_F(DriverKernelDMAChannel,
       LiveOperationsRejectReturnAndFreeWithoutMutation) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  map(transfer());
  bad(Model.planChannelReturn(C.Request.Object, dma::DeallocateObject),
      "flushing");
  bad(Model.planChannelReturn(C.Request.Object, dma::KeepObject), "system DMA");
  bad(Model.planChannelReturn(C.Request.Object, 99), "unknown");
  EXPECT_FALSE(Model.channel(C.Request.Object)->Returned);
  returned(C.Request.Object, dma::DeallocateObjectKeepRegisters);
  bad(Model.planFreeRegisters(AdapterID, C.Request.Object, 2),
      "aggregate flush");
  bad(Model.planFreeRegisters(AdapterID, C.Request.Object, 1),
      "exact retained");
  flush();
  const auto Release =
      take(Model.planFreeRegisters(AdapterID, C.Request.Object, 2));
  map(transfer());
  flush();
  bad(Model.freeRegisters(Release), "changed");
  EXPECT_NE(Model.channel(C.Request.Object), nullptr);
  good(Model.freeRegisters(
      take(Model.planFreeRegisters(AdapterID, C.Request.Object, 2))));
  EXPECT_EQ(Model.channel(C.Request.Object), nullptr);
}

TEST_F(DriverKernelDMAChannel, ReturnPlanRejectsOperationMutationBeforeCommit) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  const auto Plan = take(Model.planChannelReturn(
      C.Request.Object, dma::DeallocateObjectKeepRegisters));
  map(transfer());
  bad(Model.finishChannelReturn(Plan), "changed");
  EXPECT_FALSE(Model.channel(C.Request.Object)->Returned);
  returned(C.Request.Object, dma::DeallocateObjectKeepRegisters);
}

TEST_F(DriverKernelDMAChannel,
       StaleTransferAndFlushCannotAffectLaterOperation) {
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  const auto First = take(Model.planTransfer(transfer()));
  good(Model.commitTransfer(First));
  bad(Model.commitTransfer(First), "contiguous");
  const auto Flush = take(Model.planFlush(
      AdapterID, C.Request.Object, First.Request.MDL, First.Request.CurrentVA,
      First.Length, First.Request.Direction));
  good(Model.flush(Flush));
  map(transfer());
  bad(Model.flush(Flush), "changed");
  EXPECT_NE(Model.mapping(C.Request.Object), nullptr);
}

TEST_F(DriverKernelDMAChannel, OldLogicalAddressCannotRedirectToReusedToken) {
  start();
  const auto C = allocate();
  good(Model.beginCallback(C.Request.Object));
  const auto First = map(transfer());
  good(Model.arm({event(First.Logical, 4, 7)}, 0, 0));
  flush();
  const auto Later = map(transfer());
  EXPECT_NE(First.Logical, Later.Logical);
  bad(Model.processEvents(7), "mapping");
  ASSERT_EQ(Result.DmaTransfers.size(), 1u);
  EXPECT_TRUE(Result.DmaTransfers[0].FailureReason);
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

} // namespace
} // namespace neverd::emulation

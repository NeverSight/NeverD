//===- KernelDMATests.cpp - DMA mappings and external RAM transactions ----===//
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

class DriverKernelDMA : public ::testing::Test {
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
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
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
                 uint32_t Registers = 2) {
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
  void SetUp() override {
    configure();
    create();
    owner(1, Base, 4 * Page);
  }
};

TEST_F(DriverKernelDMA, AdapterAndCommonBufferDoNotRequireStartedHardware) {
  ASSERT_NE(Model.adapter(AdapterID), nullptr);
  EXPECT_FALSE(Resources.find(PDO)->Assigned);
  const auto Map = publish(plan(1, 64, true));
  EXPECT_EQ(Map.Logical, Logical);
  EXPECT_NE(Map.Logical, Map.Backing);
  EXPECT_NE(Map.Logical, take(Physical.physicalAddress(Map.Backing)));
  good(Model.validateGuestAccess(Base, 64, true));
  bad(Model.validateGuestAccess(Base + 63, 2, false), "requested byte count");
  bad(Model.putAdapter(AdapterID), "common buffer");
  bad(Physical.retire(1), "pinned view");
  release(Map.Object);
  good(Model.putAdapter(AdapterID));
  good(Physical.retire(1));
  EXPECT_EQ(Model.adapter(AdapterID), nullptr);
  bad(Model.validateGuestAccess(AdapterID, 1, false), "retired DMA adapter");
}

TEST_F(DriverKernelDMA, SharedDomainBudgetAndFIFOApplyAcrossAdapters) {
  create(0x2400);
  const auto A = publish(plan(1, 2 * Page));
  const auto B = publish(plan(1, Page, false, 0, 0x2400));
  const auto C = publish(plan(1, Page));
  EXPECT_TRUE(A.Live);
  EXPECT_FALSE(B.Live);
  EXPECT_FALSE(C.Live);
  EXPECT_EQ(B.Logical, A.NextLogical);
  EXPECT_EQ(C.Logical, B.NextLogical);
  bad(Model.beginCallback(B.Object), "waiting");
  good(Model.beginCallback(A.Object));
  const auto Release = take(Model.planRelease(A.Object));
  EXPECT_EQ(
      Release.Ready,
      (std::vector<KernelDMA::Promotion>{
          {KernelDMA::CallbackKind::ScatterGather, B.Object, B.SchedulerID},
          {KernelDMA::CallbackKind::ScatterGather, C.Object, C.SchedulerID}}));
  auto Incorrect = Release;
  std::reverse(Incorrect.Ready.begin(), Incorrect.Ready.end());
  bad(Model.releaseMapping(Incorrect), "changed");
  EXPECT_NE(Model.mapping(A.Object), nullptr);
  EXPECT_FALSE(Model.mapping(B.Object)->Live);
  good(Model.releaseMapping(Release));
  EXPECT_TRUE(Model.mapping(B.Object)->Live);
  EXPECT_TRUE(Model.mapping(C.Object)->Live);
  good(Model.beginCallback(B.Object));
  good(Model.beginCallback(C.Object));
  release(B.Object);
  release(C.Object);
  good(Model.finishCallback(A.Object));
  good(Model.finishCallback(B.Object));
  good(Model.finishCallback(C.Object));
}

TEST_F(DriverKernelDMA, WaitingPinsBytesAndMDLWithoutBlockingCPUUntilAdmitted) {
  owner(2, Base + 4 * Page, Page);
  const auto A = publish(plan(1, 2 * Page));
  const auto B = publish(plan(2, 16));
  good(Model.validateGuestAccess(B.Backing, 16, true));
  bad(Physical.retire(2), "pinned view");
  bad(Model.canReleaseRange(B.MDL, B.MDLSize), "MDL");
  good(Model.beginCallback(A.Object));
  release(A.Object);
  bad(Model.validateGuestAccess(B.Backing, 16, false), "PutScatterGatherList");
  good(Model.beginCallback(B.Object));
  release(B.Object);
  good(Model.validateGuestAccess(B.Backing, 16, true));
  good(Model.canReleaseRange(B.MDL, B.MDLSize));
  good(Physical.retire(2));
}

TEST_F(DriverKernelDMA, CallbackHoldSurvivesListAndAdapterRelease) {
  const auto Map = publish(plan(1, 8));
  good(Model.beginCallback(Map.Object));
  release(Map.Object);
  good(Model.putAdapter(AdapterID));
  EXPECT_EQ(Model.adapter(AdapterID), nullptr);
  EXPECT_EQ(Model.mapping(Map.Object), nullptr);
  ASSERT_NE(Model.callback(Map.Object), nullptr);
  EXPECT_EQ(Model.callback(Map.Object)->Context, 0xDEADBEEFu);
  good(Physical.retire(1));
  bad(Model.canReleaseRange(Map.Device, 1), "callback");
  bad(Model.canReleasePDO(PDO), "callback");
  good(Model.finishCallback(Map.Object));
  EXPECT_EQ(Model.callback(Map.Object), nullptr);
  good(Model.canReleaseRange(Map.Device, 1));
  good(Model.canReleasePDO(PDO));
  bad(Model.finishCallback(Map.Object), "independent owner");
}

TEST_F(DriverKernelDMA, ReturnedCallbackDoesNotReleaseItsLiveMapping) {
  const auto Map = publish(plan(1, 8));
  good(Model.beginCallback(Map.Object));
  good(Model.finishCallback(Map.Object));
  EXPECT_EQ(Model.callback(Map.Object), nullptr);
  EXPECT_NE(Model.mapping(Map.Object), nullptr);
  bad(Model.putAdapter(AdapterID), "accepted DMA map");
  bad(Physical.retire(1), "pinned view");
  release(Map.Object);
  good(Model.putAdapter(AdapterID));
  good(Physical.retire(1));
}

TEST_F(DriverKernelDMA, PacketReleaseBeforeCallbackDeliveryPreservesOwnership) {
  const auto Map = publish(plan(1, 8));
  bad(Model.planRelease(Map.Object), "callback delivery");
  bad(Model.releaseMapping({Map.Object, {}}), "callback delivery");
  EXPECT_NE(Model.mapping(Map.Object), nullptr);
  EXPECT_NE(Model.callback(Map.Object), nullptr);
  bad(Physical.retire(1), "pinned view");
  bad(Model.putAdapter(AdapterID), "accepted DMA map");
  good(Model.beginCallback(Map.Object));
  release(Map.Object);
  good(Model.finishCallback(Map.Object));
  good(Physical.retire(1));
}

TEST_F(DriverKernelDMA, CallbackPreflightsPreserveBothAdmissionAndReturnState) {
  const auto Map = publish(plan(1, 8));
  good(Model.canBeginCallback(Map.Object));
  good(Model.canBeginCallback(Map.Object));
  bad(Model.canFinishCallback(Map.Object), "independent owner");
  bad(Model.finishCallback(Map.Object), "independent owner");
  good(Model.beginCallback(Map.Object));
  bad(Model.canBeginCallback(Map.Object), "already entered");
  good(Model.canFinishCallback(Map.Object));
  good(Model.canFinishCallback(Map.Object));
  release(Map.Object);
  good(Model.putAdapter(AdapterID));
  good(Model.canFinishCallback(Map.Object));
  ASSERT_NE(Model.callback(Map.Object), nullptr);
  good(Model.finishCallback(Map.Object));
  bad(Model.canFinishCallback(Map.Object), "independent owner");
  EXPECT_EQ(Model.callback(Map.Object), nullptr);
}

TEST_F(DriverKernelDMA, PendingCallbacksIncludeResourceWaitButNotReturnedMaps) {
  EXPECT_FALSE(Model.hasPendingCallbacks());
  const auto A = publish(plan(1, 2 * Page));
  const auto B = publish(plan(1, 8));
  EXPECT_TRUE(Model.hasPendingCallbacks());
  good(Model.beginCallback(A.Object));
  good(Model.finishCallback(A.Object));
  // Only the resource waiter keeps guest callback work pending now.
  EXPECT_TRUE(Model.hasPendingCallbacks());
  release(A.Object);
  EXPECT_TRUE(Model.hasPendingCallbacks());
  good(Model.beginCallback(B.Object));
  good(Model.finishCallback(B.Object));
  EXPECT_NE(Model.mapping(B.Object), nullptr);
  EXPECT_FALSE(Model.hasPendingCallbacks());
  release(B.Object);
  EXPECT_FALSE(Model.hasPendingCallbacks());
}

TEST_F(DriverKernelDMA, ScatterGatherMetadataUsesLogicalPageFragments) {
  const auto Map = publish(plan(1, 5, false, Page - 2));
  const auto Bytes = take(Model.scatterGatherBytes(Map));
  auto Integer = [&](size_t Offset, size_t Size) {
    uint64_t Value = 0;
    for (size_t I = 0; I < Size; ++I)
      Value |= uint64_t(Bytes[Offset + I]) << (I * 8);
    return Value;
  };
  ASSERT_EQ(Bytes.size(), 64u);
  EXPECT_EQ(Integer(0, 4), 2u);
  EXPECT_EQ(Integer(8, 8), 0u);
  EXPECT_EQ(Integer(16, 8), Logical + Page - 2);
  EXPECT_EQ(Integer(24, 4), 2u);
  EXPECT_EQ(Integer(40, 8), Logical + Page);
  EXPECT_EQ(Integer(48, 4), 3u);
  good(Model.validateGuestAccess(Map.Object, uint32_t(Bytes.size()), false));
  bad(Model.validateGuestAccess(Map.Object + 16, 8, true), "model-owned");
}

TEST_F(DriverKernelDMA, StalePreparationAndImpossibleLengthHaveNoPinsOrCharge) {
  auto A = plan(1, 8);
  const auto B = publish(plan(1, 8));
  bad(Model.publishMapping(A), "logical identity");
  EXPECT_EQ(Model.mapping(A.Object), nullptr);
  EXPECT_FALSE(take(Model.planMapping(AdapterID, 1, 0, 3 * Page, false,
                                      DriverDmaDirection::ReadMemory)));
  good(Model.beginCallback(B.Object));
  release(B.Object);
  const auto C = publish(plan(1, 8));
  EXPECT_EQ(C.Logical, B.NextLogical);
  EXPECT_TRUE(C.Live);
}

TEST_F(DriverKernelDMA, CommonBufferReleasePreflightsOtherAllocationPins) {
  const auto Map = publish(plan(1, 16, true));
  const auto ExtraPin = take(Physical.pin(1, 8, 8));
  bad(Model.planRelease(Map.Object), "pinned view");
  EXPECT_NE(Model.mapping(Map.Object), nullptr);
  EXPECT_FALSE(take(Model.planMapping(AdapterID, 1, 0, 2 * Page, true,
                                      DriverDmaDirection::ReadMemory)));
  good(Physical.unpin(ExtraPin));
  release(Map.Object);
  good(Physical.retire(1));
}

TEST_F(DriverKernelDMA,
       ArmDoesNotRequireMappingAndPreservesScenarioSourceIndex) {
  start();
  Result.Requests.resize(10); // Includes non-scenario power children.
  good(Model.canArm({event(Logical + 2, 3, 7)}, 4, 100));
  EXPECT_TRUE(Result.DmaTransfers.empty());
  good(Model.arm({event(Logical + 2, 3, 7)}, 4, 100));
  ASSERT_EQ(Result.DmaTransfers.size(), 1u);
  EXPECT_EQ(Result.DmaTransfers[0].SourceRequestIndex, 4u);
  EXPECT_EQ(Result.DmaTransfers[0].Epoch, 1u);
  EXPECT_EQ(Model.nextEventTime(), 107u);
  const auto Map = publish(plan(1, 16, true));
  good(Memory.write(Base + 2, {1, 2, 3}));
  good(Model.processEvents(106));
  EXPECT_EQ(Memory.DeviceReads, 0u);
  good(Model.processEvents(107));
  EXPECT_EQ(Result.DmaTransfers[0].Data, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(Result.DmaTransfers[0].Mapping, Map.Object);
  EXPECT_EQ(Result.DmaTransfers[0].Adapter, AdapterID);
  EXPECT_EQ(Result.DmaTransfers[0].CompletedAt100ns, 107u);
  EXPECT_FALSE(Model.hasPendingEvents());
  EXPECT_FALSE(Model.nextEventTime());
}

TEST_F(DriverKernelDMA, DeadlineThenSubmissionOrderControlsSharedRAMEffects) {
  start();
  publish(plan(1, 16, true));
  good(Model.arm({event(Logical, 2, 10), event(Logical, 2, 5, {7, 8}),
                  event(Logical, 2, 10, {9, 10})},
                 0, 0));
  good(Model.processEvents(10));
  EXPECT_EQ(Result.DmaTransfers[0].Data, (std::vector<uint8_t>{7, 8}));
  EXPECT_EQ(Result.DmaTransfers[1].Data, (std::vector<uint8_t>{7, 8}));
  EXPECT_EQ(Result.DmaTransfers[2].Data, (std::vector<uint8_t>{9, 10}));
  EXPECT_EQ(Memory.Bytes[0], 9);
  EXPECT_EQ(Memory.Bytes[1], 10);
  EXPECT_EQ(Memory.DeviceReads, 1u);
  EXPECT_EQ(Memory.DeviceWrites, 2u);
}

TEST_F(DriverKernelDMA,
       PacketDirectionFailureLeavesRAMAndLaterEventsUntouched) {
  start();
  const auto Map = publish(plan(1, 8));
  good(Model.arm({event(Map.Logical, 2, 0, {1, 2}), event(Map.Logical, 2)}, 0,
                 0));
  bad(Model.processEvents(0), "direction");
  EXPECT_EQ(Memory.DeviceReads, 0u);
  EXPECT_EQ(Memory.DeviceWrites, 0u);
  EXPECT_EQ(Memory.Bytes[0], 0u);
  ASSERT_TRUE(Result.DmaTransfers[0].FailureReason);
  EXPECT_EQ(Result.DmaTransfers[0].OccurredAt100ns, 0u);
  EXPECT_FALSE(Result.DmaTransfers[0].CompletedAt100ns);
  EXPECT_TRUE(Result.DmaTransfers[0].Data.empty());
  EXPECT_FALSE(Result.DmaTransfers[1].OccurredAt100ns);
}

TEST_F(DriverKernelDMA, PacketWriteUsesPinnedBackingAcrossPageFragments) {
  start();
  const auto Map = publish(
      plan(1, 5, false, Page - 2, AdapterID, DriverDmaDirection::WriteMemory));
  good(Model.beginCallback(Map.Object));
  good(Model.arm({event(Map.Logical + 1, 3, 5, {4, 5, 6})}, 0, 0));
  good(Model.processEvents(5));
  EXPECT_EQ(Memory.Bytes[Page - 2], 0);
  EXPECT_EQ(Memory.Bytes[Page - 1], 4);
  EXPECT_EQ(Memory.Bytes[Page], 5);
  EXPECT_EQ(Memory.Bytes[Page + 1], 6);
  EXPECT_EQ(Memory.Bytes[Page + 2], 0);
  EXPECT_EQ(Memory.DeviceWrites, 1u);
  bad(Model.validateGuestAccess(Map.Backing, Map.Length, false),
      "PutScatterGatherList");
  release(Map.Object);
  good(Model.validateGuestAccess(Map.Backing, Map.Length, false));
  good(Model.finishCallback(Map.Object));
}

TEST_F(DriverKernelDMA, PacketReadObservesGuestWritesWithoutADuplicateRAMBank) {
  start();
  good(Memory.write(Base + 8, {11, 12, 13}));
  const auto Map = publish(plan(1, 16));
  good(Model.arm({event(Map.Logical + 8, 3)}, 0, 0));
  good(Model.processEvents(0));
  EXPECT_EQ(Result.DmaTransfers[0].Data, (std::vector<uint8_t>{11, 12, 13}));
  EXPECT_EQ(Memory.DeviceReads, 1u);
  EXPECT_EQ(Memory.DeviceWrites, 0u);
}

TEST_F(DriverKernelDMA, CrossMapWriteNeverTouchesEitherValidPrefix) {
  start();
  owner(2, Base + 4 * Page, Page);
  publish(plan(1, Page, true));
  publish(plan(2, Page, true));
  good(Model.arm({event(Logical + Page - 1, 2, 0, {0xAA, 0xBB})}, 0, 0));
  bad(Model.processEvents(0), "one complete live logical mapping");
  EXPECT_EQ(Memory.DeviceWrites, 0u);
  EXPECT_EQ(Memory.Bytes[Page - 1], 0);
  EXPECT_EQ(Memory.Bytes[4 * Page], 0);
}

TEST_F(DriverKernelDMA, WaitingMappingCannotReceiveExternalTransactions) {
  start();
  publish(plan(1, 2 * Page));
  const auto Waiting = publish(plan(1, 8));
  ASSERT_FALSE(Waiting.Live);
  good(Model.arm({event(Waiting.Logical, 2)}, 0, 0));
  bad(Model.processEvents(0), "live logical mapping");
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

TEST_F(DriverKernelDMA, ReleasedLogicalAddressesNeverRetargetNewMappings) {
  start();
  const auto First = publish(plan(1, 8, true));
  good(Model.arm({event(First.Logical, 1, 10)}, 0, 0));
  release(First.Object);
  owner(2, Base + 4 * Page, Page);
  const auto Second = publish(plan(2, 8, true));
  EXPECT_GT(Second.Logical, First.Logical);
  bad(Model.processEvents(10), "live logical mapping");
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

TEST_F(DriverKernelDMA, PendingTransactionOwnsPDOAfterAdapterRetirement) {
  start();
  good(Model.arm({event(Logical, 1, 8)}, 0, 0));
  good(Model.putAdapter(AdapterID));
  bad(Model.canReleasePDO(PDO), "explicit DMA transaction");
  EXPECT_EQ(Model.nextEventTime(), 8u);
  bad(Model.processEvents(8), "live logical mapping");
  EXPECT_TRUE(Result.DmaTransfers[0].FailureReason);
}

TEST_F(DriverKernelDMA, PDOLogicalDomainsDoNotAliasEqualAddresses) {
  configure(PDO + 1, "other");
  create(0x2400, PDO + 1);
  start();
  start(PDO + 1);
  owner(2, Base + 4 * Page, Page);
  publish(plan(1, 8, true));
  const auto Other = publish(plan(2, 8, true, 0, 0x2400));
  EXPECT_EQ(Other.Logical, Logical);
  good(Memory.write(Base, {1, 2}));
  good(Memory.write(Base + 4 * Page, {3, 4}));
  auto Input = event(Logical, 2);
  Input.DeviceID = "other";
  good(Model.arm({Input, event(Logical, 2)}, 0, 0));
  good(Model.processEvents(0));
  EXPECT_EQ(Result.DmaTransfers[0].Data, (std::vector<uint8_t>{3, 4}));
  EXPECT_EQ(Result.DmaTransfers[1].Data, (std::vector<uint8_t>{1, 2}));
}

TEST_F(DriverKernelDMA, PhysicalD3FailureCannotPerformAPrefixWrite) {
  start();
  publish(plan(1, 8, true));
  good(Model.arm({event(Logical, 2, 5, {1, 2})}, 0, 0));
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  bad(Model.processEvents(5), "physical device power D0");
  EXPECT_EQ(Memory.DeviceWrites, 0u);
  EXPECT_EQ(Memory.Bytes[0], 0u);
  EXPECT_EQ(Result.DmaTransfers[0].OccurredAt100ns, 5u);
}

TEST_F(DriverKernelDMA, DeliveryUsesActualD0StateRatherThanArmTimePower) {
  start();
  publish(plan(1, 8, true));
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  good(Model.arm({event(Logical, 2, 5, {1, 2})}, 0, 0));
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  good(Model.processEvents(5));
  EXPECT_EQ(Memory.Bytes[0], 1u);
  EXPECT_EQ(Memory.Bytes[1], 2u);
}

TEST_F(DriverKernelDMA, SurpriseRemovalRejectsAlreadyArmedTransaction) {
  start();
  publish(plan(1, 8, true));
  good(Model.arm({event(Logical, 2, 5)}, 0, 0));
  Resources.surpriseRemoval(PDO);
  bad(Model.processEvents(5), "unavailable resource epoch");
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

TEST_F(DriverKernelDMA, RestartCannotRetargetAnOldAssignmentEvent) {
  start();
  good(Model.arm({event(Logical, 2, 5)}, 0, 0));
  // The coordinator normally forbids teardown with pending events. Exercise
  // the event's own epoch check independently of that enclosing preflight.
  good(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  start();
  publish(plan(1, 8, true));
  bad(Model.processEvents(5), "unavailable resource epoch");
  EXPECT_EQ(Result.DmaTransfers[0].Epoch, 1u);
  EXPECT_EQ(Resources.find(PDO)->Epoch, 2u);
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

TEST_F(DriverKernelDMA, WholeBackingValidationPrecedesAnyDeviceWrite) {
  start();
  publish(plan(1, 2 * Page, true));
  good(Model.arm({event(Logical + Page - 1, 2, 0, {7, 8})}, 0, 0));
  Memory.Hole = Base + Page;
  bad(Model.processEvents(0), "backing hole");
  EXPECT_EQ(Memory.DeviceWrites, 0u);
  EXPECT_EQ(Memory.Bytes[Page - 1], 0u);
  EXPECT_EQ(Memory.Bytes[Page], 0u);
  EXPECT_TRUE(Result.DmaTransfers[0].Data.empty());
}

TEST_F(DriverKernelDMA, ArmBatchFailureDoesNotPublishAnyObservation) {
  start();
  auto Bad = event(Logical, 1);
  Bad.DeviceID = "missing";
  bad(Model.arm({event(Logical, 1), Bad}, 0, 0), "unknown DMA device");
  bad(Model.arm({event(Logical, 1)}, uint64_t(UINT32_MAX) + 1, 0), "capacity");
  bad(Model.arm({event(Logical, 1, 1)}, 0, UINT64_MAX), "time range");
  Bad = event(Logical, 2, 0, {1});
  bad(Model.arm({Bad}, 0, 0), "exact byte count");
  EXPECT_TRUE(Result.DmaTransfers.empty());
  EXPECT_FALSE(Model.nextEventTime());
  good(Model.arm({event(Logical, 1)}, 7, 9));
  ASSERT_EQ(Result.DmaTransfers.size(), 1u);
  EXPECT_EQ(Result.DmaTransfers[0].EventIndex, 0u);
  EXPECT_EQ(Result.DmaTransfers[0].SourceRequestIndex, 7u);
}

TEST_F(DriverKernelDMA, ObservationByteBudgetIsReservedBeforeDelivery) {
  start();
  const auto Large = event(Logical, DriverDmaMaximumLengthLimit);
  std::vector<DriverDmaEvent> Inputs(16, Large);
  good(Model.arm(Inputs, 0, 0));
  bad(Model.arm({event(Logical, 1)}, 1, 0), "byte limit");
  EXPECT_EQ(Result.DmaTransfers.size(), 16u);
  EXPECT_EQ(Memory.DeviceReads, 0u);
}

TEST_F(DriverKernelDMA, EarlierCompletedEventSurvivesLaterDeliveryFailure) {
  start();
  publish(plan(1, 8, true));
  good(Model.arm({event(Logical, 1, 0, {4}), event(Logical + Page, 1, 1),
                  event(Logical, 1, 2, {9})},
                 0, 0));
  bad(Model.processEvents(2), "live logical mapping");
  EXPECT_EQ(Memory.Bytes[0], 4u);
  EXPECT_EQ(Result.DmaTransfers[0].CompletedAt100ns, 2u);
  EXPECT_EQ(Result.DmaTransfers[1].OccurredAt100ns, 2u);
  EXPECT_TRUE(Result.DmaTransfers[1].FailureReason);
  EXPECT_FALSE(Result.DmaTransfers[2].OccurredAt100ns);
  EXPECT_EQ(Model.nextEventTime(), 1u);
}

} // namespace
} // namespace neverd::emulation

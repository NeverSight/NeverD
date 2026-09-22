//===- KernelRemoveLockBridgeTests.cpp - Remove-lock guest ABI boundaries -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise extension provenance, guest storage opacity, IRQL and transactional
/// device teardown through KernelModel's real imported API boundary.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelRemoveLockBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint64_t ExtensionSize = 256;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0;

  void ok(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error Error, llvm::StringRef Text) {
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    reject(Value.takeError(), Text);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  uint64_t create() {
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), ExtensionSize, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    const uint64_t Device = get(Scratch);
    put(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    return Device;
  }
  uint64_t extension(uint64_t Device) {
    return get(Device + DeviceExtensionOffset);
  }
  void initialize(uint64_t Lock, uint32_t Size) {
    EXPECT_EQ(call("IoInitializeRemoveLockEx", {Lock, 0, 0, 0, Size}), 0u);
  }
  uint64_t acquire(uint64_t Lock, uint32_t Size, uint64_t Tag = 0) {
    return call("IoAcquireRemoveLockEx", {Lock, Tag, 0, 0, Size});
  }
  void release(uint64_t Lock, uint32_t Size, uint64_t Tag = 0) {
    EXPECT_EQ(call("IoReleaseRemoveLockEx", {Lock, Tag, Size}), 0u);
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "remove0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Options.PnpDevices.push_back(Device);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        Entry);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("remove0")).Argument1;
    ASSERT_NE(PDO, 0u);
  }
};

TEST_F(KernelRemoveLockBridge,
       RetailAndDebugLocksInitializeBeforeAttachmentWithIndependentOwners) {
  const uint64_t First = create(), Second = create();
  const uint64_t Retail = extension(First) + 8;
  const uint64_t Debug = extension(Second) + 8;
  initialize(Retail, remove_lock::RetailSize);
  initialize(Debug, remove_lock::DebugSize);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {First, PDO}), PDO);
  ok(Model->finishAddDevice("remove0", StatusSuccess));
  EXPECT_EQ(acquire(Retail, remove_lock::RetailSize), StatusSuccess);
  EXPECT_EQ(acquire(Debug, remove_lock::DebugSize), StatusSuccess);
  release(Retail, remove_lock::RetailSize);
  reject(Model->call("IoDeleteDevice", {Second}), "acquisitions");
  release(Debug, remove_lock::DebugSize);
  call("IoDeleteDevice", {Second});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {First});
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  reject(Model->call("IoAcquireRemoveLockEx",
                     {Debug, 0, 0, 0, remove_lock::DebugSize}),
         "unknown");
}

TEST_F(KernelRemoveLockBridge,
       RegisteredBytesAndInternalEventAreOpaqueButSurroundingsRemainWritable) {
  const uint64_t Device = create();
  const uint64_t Base = extension(Device);
  const uint64_t Lock = Base + 8;
  initialize(Lock, remove_lock::DebugSize);
  for (bool Write : {false, true}) {
    reject(Model->validateGuestAccess(Lock, 1, Write), "opaque");
    reject(
        Model->validateGuestAccess(Lock + remove_lock::DebugSize - 1, 1, Write),
        "opaque");
    reject(Model->validateGuestAccess(Lock - 1, 2, Write), "opaque");
  }
  put(Base, 0x12345678, 8);
  put(Lock + remove_lock::DebugSize, 0x87654321, 8);
  EXPECT_EQ(get(Base), 0x12345678u);
  EXPECT_EQ(get(Lock + remove_lock::DebugSize), 0x87654321u);
  reject(Model->call("KeInitializeEvent", {Lock + 8, 0, 0}), "opaque");
  reject(Model->call("KeWaitForSingleObject", {Lock + 8, 0, 0, 0, 0}),
         "initialized");
  EXPECT_FALSE(Model->takeWait());
  EXPECT_EQ(acquire(Lock, remove_lock::DebugSize, 0xdeadbeef), StatusSuccess);
  release(Lock, remove_lock::DebugSize, 0xdeadbeef);
}

TEST_F(KernelRemoveLockBridge,
       InvalidProvenanceExtentAndMetadataDoNotRegisterStorage) {
  const uint64_t Device = create();
  const uint64_t Base = extension(Device);
  const uint64_t Pool = call("ExAllocatePoolWithTag", {0, 256, 0x526c});
  for (const uint64_t Address :
       {Device, Scratch, Pool, Base - 8, Base + ExtensionSize - 8, PDO})
    reject(Model->call("IoInitializeRemoveLockEx",
                       {Address, 0, 0, 0, remove_lock::RetailSize}),
           "extension");
  reject(Model->call("IoInitializeRemoveLockEx",
                     {Base + 1, 0, 0, 0, remove_lock::RetailSize}),
         "unaligned");
  reject(Model->call("IoInitializeRemoveLockEx",
                     {UINT64_MAX - 7, 0, 0, 0, remove_lock::RetailSize}),
         "overflowing");
  reject(Model->call("IoInitializeRemoveLockEx", {Base, 0, 0, 0, 33}), "size");
  reject(Model->call("IoInitializeRemoveLockEx",
                     {Base, 0, 0, 0x80000000, remove_lock::RetailSize}),
         "HighWatermark");
  ok(Model->validateGuestAccess(Base, ExtensionSize, true));
  EXPECT_EQ(call("IoInitializeRemoveLockEx",
                 {Base, 0, 0xffffffff, 0x7fffffff, remove_lock::RetailSize}),
            0u);
  EXPECT_EQ(acquire(Base, remove_lock::RetailSize), StatusSuccess);
  release(Base, remove_lock::RetailSize);
  call("ExFreePoolWithTag", {Pool, 0x526c});
}

TEST_F(KernelRemoveLockBridge,
       SizeAndOverlapRejectionsPreserveExistingAcquisition) {
  const uint64_t Device = create();
  const uint64_t Lock = extension(Device);
  initialize(Lock, remove_lock::RetailSize);
  EXPECT_EQ(acquire(Lock, remove_lock::RetailSize, 0x123), StatusSuccess);
  reject(Model->call("IoInitializeRemoveLockEx",
                     {Lock, 0, 0, 0, remove_lock::RetailSize}),
         "opaque");
  reject(Model->call("IoInitializeRemoveLockEx",
                     {Lock + 16, 0, 0, 0, remove_lock::DebugSize}),
         "opaque");
  reject(Model->call("IoAcquireRemoveLockEx",
                     {Lock, 0x123, 0, 0, remove_lock::DebugSize}),
         "size");
  reject(Model->call("IoReleaseRemoveLockEx",
                     {Lock, 0x123, remove_lock::DebugSize}),
         "size");
  reject(Model->call("IoReleaseRemoveLockAndWaitEx",
                     {Lock, 0x123, remove_lock::RetailSize}),
         "provider receipt");
  EXPECT_FALSE(Model->takeWait());
  // Rejected AndWait neither closes admission nor consumes its acquisition.
  EXPECT_EQ(acquire(Lock, remove_lock::RetailSize, 0x123), StatusSuccess);
  release(Lock, remove_lock::RetailSize, 0x123);
  reject(Model->call("IoDeleteDevice", {Device}), "acquisitions");
  release(Lock, remove_lock::RetailSize, 0x123);
  call("IoDeleteDevice", {Device});
  reject(Model->validateGuestAccess(Lock, 1, false), "freed");
}

TEST_F(KernelRemoveLockBridge,
       HeldLockRejectsDeleteAndDetachBeforeDeviceOrTopologyMutation) {
  const uint64_t Device = create();
  const uint64_t Lock = extension(Device);
  initialize(Lock, remove_lock::RetailSize);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Device, PDO}), PDO);
  ok(Model->finishAddDevice("remove0", StatusSuccess));
  EXPECT_EQ(acquire(Lock, remove_lock::RetailSize), StatusSuccess);
  const uint64_t StackCount = get(Device + DeviceStackCountOffset, 1);
  reject(Model->call("IoDeleteDevice", {Device}), "acquisitions");
  reject(Model->call("IoDetachDevice", {PDO}), "acquisitions");
  ok(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_TRUE(Result.PnpDevices.front().Attached);
  EXPECT_EQ(get(Device + DeviceStackCountOffset, 1), StackCount);
  // A fresh work item proves failed delete did not publish DeletePending.
  const uint64_t Work = call("IoAllocateWorkItem", {Device});
  ASSERT_NE(Work, 0u);
  call("IoFreeWorkItem", {Work});
  release(Lock, remove_lock::RetailSize);
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {Device});
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_FALSE(Result.PnpDevices.front().Attached);
}

TEST_F(KernelRemoveLockBridge,
       CleanFailedAddDeviceRetiresInitializedUnusedRetailAndDebugLocks) {
  const uint64_t Device = create();
  const uint64_t Base = extension(Device);
  initialize(Base, remove_lock::RetailSize);
  initialize(Base + remove_lock::RetailSize, remove_lock::DebugSize);
  call("IoDeleteDevice", {Device});
  ok(Model->finishAddDevice("remove0", 0xc0000001));
  EXPECT_TRUE(Result.Devices.empty());
  ASSERT_EQ(Result.PnpDevices.size(), 1u);
  EXPECT_EQ(Result.PnpDevices.front().AddDeviceStatus, 0xc0000001u);
  EXPECT_FALSE(Result.PnpDevices.front().ProviderPresent);
  reject(Model->validateGuestAccess(Base, 1, false), "freed");
  reject(Model->call("IoAcquireRemoveLockEx",
                     {Base, 0, 0, 0, remove_lock::RetailSize}),
         "unknown");
}

TEST_F(KernelRemoveLockBridge,
       DpcAcquisitionIgnoresFilePointerAndPassiveOnlyFailuresAreAtomic) {
  const uint64_t Device = create();
  const uint64_t Lock = extension(Device);
  initialize(Lock, remove_lock::RetailSize);
  const uint64_t Dpc = Scratch + 0x200;
  call("KeInitializeDpc", {Dpc, Entry, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto Scheduled = take(Model->nextScheduled(false));
  ASSERT_TRUE(Scheduled);
  ASSERT_EQ(Model->currentIRQL(), scheduler::DispatchLevel);
  EXPECT_EQ(call("IoAcquireRemoveLockEx",
                 {Lock, 0, UINT64_MAX, 0xffffffff, remove_lock::RetailSize}),
            StatusSuccess);
  reject(Model->call("IoInitializeRemoveLockEx",
                     {Lock + 64, 0, 0, 0, remove_lock::RetailSize}),
         "IRQL");
  reject(Model->call("IoReleaseRemoveLockAndWaitEx",
                     {Lock, 0, remove_lock::RetailSize}),
         "IRQL");
  release(Lock, remove_lock::RetailSize);
  ok(Model->finishScheduled(Scheduled->ID));
  EXPECT_EQ(Model->currentIRQL(), scheduler::PassiveLevel);
  EXPECT_FALSE(Model->takeWait());
  initialize(Lock + 64, remove_lock::RetailSize);
  EXPECT_EQ(acquire(Lock, remove_lock::RetailSize), StatusSuccess);
  release(Lock, remove_lock::RetailSize);
  call("IoDeleteDevice", {Device});
}
} // namespace
} // namespace neverd::emulation

//===- KernelFrameworkControlTests.cpp - Control-device failure ownership
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check initializer consumption and rollback across the WDF allocation and
/// authoritative WDM device publication boundary.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkQueueTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;

class DriverKernelFrameworkControl : public DriverKernelFrameworkQueue {
protected:
  uint64_t initializer(bool Named = true) {
    string(Security, "D:P(A;;GA;;;WD)");
    const auto Init = take(invoke("WdfControlDeviceInitAllocate",
                                  {Globals, DriverHandle, Security}));
    put(InitSlot, Init);
    if (Named) {
      string(DeviceName, "\\Device\\QueueTestControl");
      EXPECT_EQ(
          take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName})),
          0u);
    }
    queueAttributes();
    put(DeviceSlot, Sentinel);
    return Init;
  }

  llvm::Expected<uint64_t> createControl() {
    return invoke("WdfDeviceCreate", {Globals, InitSlot, Attrs, DeviceSlot});
  }
};

TEST_F(DriverKernelFrameworkControl,
       SuccessfulCreationConsumesInitializerAndKeepsDistinctIdentity) {
  const auto Init = initializer();
  const size_t Published = HostDevices.size();
  ASSERT_EQ(take(createControl()), 0u);
  const auto Handle = get(DeviceSlot);
  ASSERT_NE(Handle, 0u);
  EXPECT_EQ(get(InitSlot), 0u);
  const auto Wdm =
      take(invoke("WdfDeviceWdmGetDeviceObject", {Globals, Handle}));
  EXPECT_NE(Handle, Wdm);
  EXPECT_EQ(HostDevices.size(), Published + 1);
  EXPECT_TRUE(HostDevices.count(Wdm));
  expectError(Model.validateGuestAccess(Init, 1, false), "freed");
  expectError(invoke("WdfDeviceInitFree", {Globals, Init}), "consumed");
  expectError(invoke("WdfDeviceInitSetIoType", {Globals, Init, 2}), "consumed");
  take(invoke("WdfObjectDelete", {Globals, Handle}));
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_FALSE(HostDevices.count(Wdm));
}

TEST_F(DriverKernelFrameworkControl,
       HandleAllocationFailureRollsBackPublishedDeviceAndCanRetry) {
  const auto Init = initializer();
  const size_t Published = HostDevices.size();
  const size_t Live = liveAllocations();
  FailAllocation = AllocationAttempts + 1;
  expectError(createControl(), "injected allocation failure");
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_EQ(liveAllocations(), Live);
  EXPECT_EQ(get(InitSlot), Init);
  EXPECT_EQ(get(DeviceSlot), 0u);
  EXPECT_FALSE(Released.count(Init));
  FailAllocation = 0;
  EXPECT_EQ(take(createControl()), 0u);
  EXPECT_EQ(HostDevices.size(), Published + 1);
  EXPECT_EQ(get(InitSlot), 0u);
  EXPECT_TRUE(Released.count(Init));
}

TEST_F(DriverKernelFrameworkControl,
       ContextAllocationFailureReleasesHandleAndAllowsInitializerRetry) {
  const auto Init = initializer();
  type();
  queueAttributes(0, Type);
  const size_t Published = HostDevices.size();
  const size_t Live = liveAllocations();
  FailAllocation = AllocationAttempts + 2;
  expectError(createControl(), "injected allocation failure");
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_EQ(liveAllocations(), Live);
  EXPECT_EQ(get(InitSlot), Init);
  EXPECT_EQ(get(DeviceSlot), 0u);
  EXPECT_FALSE(Released.count(Init));
  FailAllocation = 0;
  EXPECT_EQ(take(createControl()), 0u);
  const auto Handle = get(DeviceSlot);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type}));
  ASSERT_NE(Context, 0u);
  EXPECT_EQ(get(Context), 0u);
  EXPECT_EQ(HostDevices.size(), Published + 1);
  EXPECT_EQ(get(InitSlot), 0u);
}

TEST_F(DriverKernelFrameworkControl,
       ObjectLimitFailureCannotLeaveAnUnownedPublishedDevice) {
  const auto Init = initializer();
  std::vector<uint64_t> Objects;
  // SetUp owns one driver and one control device before these generic objects.
  for (size_t I = 2; I < framework::MaxObjects; ++I)
    Objects.push_back(object());
  const size_t Published = HostDevices.size();
  const size_t Live = liveAllocations();
  expectError(createControl(), "object limit exhausted");
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_EQ(liveAllocations(), Live);
  EXPECT_EQ(get(InitSlot), Init);
  EXPECT_EQ(get(DeviceSlot), 0u);
  ASSERT_FALSE(Objects.empty());
  take(invoke("WdfObjectDelete", {Globals, Objects.back()}));
  EXPECT_EQ(take(createControl()), 0u);
  EXPECT_EQ(HostDevices.size(), Published + 1);
  EXPECT_EQ(get(InitSlot), 0u);
}

TEST_F(DriverKernelFrameworkControl,
       InvalidOutputPointersCannotPublishOrConsumeAnInitializer) {
  const auto Init = initializer();
  const size_t Attempts = AllocationAttempts;
  const size_t Published = HostDevices.size();
  for (uint64_t Address : {DeviceSlot, InitSlot}) {
    SCOPED_TRACE(Address);
    DenyWriteAt = Address;
    expectError(createControl(), "write validation");
    EXPECT_EQ(AllocationAttempts, Attempts);
    EXPECT_EQ(HostDevices.size(), Published);
    EXPECT_EQ(get(InitSlot), Init);
    EXPECT_FALSE(Released.count(Init));
  }
  DenyWriteAt = 0;
  EXPECT_EQ(take(createControl()), 0u);
  EXPECT_EQ(get(InitSlot), 0u);
}

TEST_F(DriverKernelFrameworkControl,
       EmptyAndClearedNamesPreserveTheInitializerForLaterAssignment) {
  const auto Init = initializer(false);
  const size_t Attempts = AllocationAttempts;
  const size_t Published = HostDevices.size();
  expectError(createControl(), "unnamed");
  string(DeviceName, "\\Device\\QueueTestControl");
  take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName}));
  take(invoke("WdfDeviceInitAssignName", {Globals, Init, 0}));
  expectError(createControl(), "unnamed");
  string(DeviceName, "");
  take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName}));
  expectError(createControl(), "unnamed");
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_EQ(get(InitSlot), Init);
  string(DeviceName, "\\Device\\QueueTestControl");
  take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName}));
  EXPECT_EQ(take(createControl()), 0u);
}

TEST_F(DriverKernelFrameworkControl,
       UnsupportedSecurityAndMalformedStringsFailBeforeAllocation) {
  const size_t Attempts = AllocationAttempts;
  const size_t Published = HostDevices.size();
  for (llvm::StringRef Descriptor : {"D:P(A;;GA;;;SY)", "D:", ""}) {
    SCOPED_TRACE(Descriptor.str());
    string(Security, Descriptor);
    expectError(invoke("WdfControlDeviceInitAllocate",
                       {Globals, DriverHandle, Security}),
                "only D:P(A;;GA;;;WD)");
  }
  string(Security, "D:P(A;;GA;;;WD)");
  put(Security, 1, 2);
  expectError(
      invoke("WdfControlDeviceInitAllocate", {Globals, DriverHandle, Security}),
      "counted Unicode");
  string(Security, "D:P(A;;GA;;;WD)");
  put(Security + 2, 0, 2);
  expectError(
      invoke("WdfControlDeviceInitAllocate", {Globals, DriverHandle, Security}),
      "counted Unicode");
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(HostDevices.size(), Published);
}

TEST_F(DriverKernelFrameworkControl,
       ExplicitFreeInvalidatesInitializerBeforeAnyDeviceIsPublished) {
  const auto Init = initializer();
  const size_t Published = HostDevices.size();
  take(invoke("WdfDeviceInitFree", {Globals, Init}));
  EXPECT_TRUE(Released.count(Init));
  expectError(invoke("WdfDeviceInitFree", {Globals, Init}), "initializer");
  expectError(createControl(), "live device initializer");
  EXPECT_EQ(get(InitSlot), Init);
  EXPECT_EQ(get(DeviceSlot), 0u);
  EXPECT_EQ(HostDevices.size(), Published);
}

TEST_F(DriverKernelFrameworkControl,
       WdmCreationStatusPreservesInitializerAndCanBeRetried) {
  const auto Init = initializer();
  const size_t Published = HostDevices.size();
  const size_t Attempts = AllocationAttempts;
  unsigned HostAttempts = 0;
  KernelFramework::DeviceHost Host;
  Host.Create = [this, &HostAttempts](llvm::StringRef, uint32_t)
      -> llvm::Expected<KernelFramework::DeviceCreation> {
    if (++HostAttempts == 1)
      return KernelFramework::DeviceCreation{windows::StatusObjectNameCollision,
                                             0};
    const auto Address = NextHostDevice;
    NextHostDevice += 0x100;
    HostDevices.insert(Address);
    return KernelFramework::DeviceCreation{0, Address};
  };
  Host.Delete = [this](uint64_t Address) {
    return HostDevices.erase(Address) == 1
               ? llvm::Error::success()
               : failure("unknown host device deletion");
  };
  Model.setDeviceHost(std::move(Host));
  EXPECT_EQ(take(createControl()), windows::StatusObjectNameCollision);
  EXPECT_EQ(get(InitSlot), Init);
  EXPECT_EQ(get(DeviceSlot), 0u);
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(HostDevices.size(), Published);
  EXPECT_EQ(take(createControl()), 0u);
  EXPECT_EQ(get(InitSlot), 0u);
  EXPECT_EQ(HostDevices.size(), Published + 1);
  EXPECT_EQ(HostAttempts, 2u);
}

} // namespace
} // namespace neverd::emulation

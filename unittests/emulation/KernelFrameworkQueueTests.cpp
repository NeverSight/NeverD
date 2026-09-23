//===- KernelFrameworkQueueTests.cpp - KMDF queue ownership contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise public queue APIs through real binding, driver and control-device
/// creation. The typed host bridge supplies only underlying WDM identities.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkQueueTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;

TEST_F(DriverKernelFrameworkQueue,
       DefaultQueueAndDeviceKeepDistinctIdentities) {
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), 0u);
  put(QueueConfig + 88, DriverHandle);
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Queue = get(QueueSlot);
  EXPECT_NE(Queue, Device);
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), Queue);
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Queue})), Device);
  const auto Wdm =
      take(invoke("WdfDeviceWdmGetDeviceObject", {Globals, Device}));
  EXPECT_NE(Wdm, Device);
  EXPECT_NE(Wdm, Queue);
  EXPECT_TRUE(HostDevices.count(Wdm));
  expectError(Model.validateGuestAccess(Queue, 1, false), "opaque");
  EXPECT_FALSE(Model.takeGuestCall());
}

TEST_F(DriverKernelFrameworkQueue, DefaultQueueMayOmitItsOutputPointer) {
  EXPECT_EQ(take(createQueue(0)), 0u);
  const auto Queue =
      take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device}));
  ASSERT_NE(Queue, 0u);
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Queue})), Device);
  EXPECT_EQ(get(QueueSlot), Sentinel);
}

TEST_F(DriverKernelFrameworkQueue, FailedConfigurationsLeaveNoDefaultQueue) {
  struct Case {
    uint64_t Offset;
    uint64_t Value;
    unsigned Width;
    uint32_t Status;
  };
  const Case Cases[] = {{0, 95, 4, 0xc0000004}, {4, 0, 4, 0xc000000d},
                        {4, 4, 4, 0xc000000d},  {4, 3, 4, 0xc000000d},
                        {40, 0, 8, 0xc020020b}, {80, 1, 4, 0xc000000d}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Offset);
    SCOPED_TRACE(Case.Value);
    queueConfiguration();
    put(QueueConfig + Case.Offset, Case.Value, Case.Width);
    const size_t Attempts = AllocationAttempts;
    EXPECT_EQ(take(createQueue()), Case.Status);
    EXPECT_EQ(AllocationAttempts, Attempts);
    EXPECT_EQ(get(QueueSlot), Sentinel);
    EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), 0u);
  }
  queueConfiguration();
  put(QueueConfig + 13, 0, 1);
  EXPECT_EQ(take(createQueue(0)), 0xc00000f2u);
}

TEST_F(DriverKernelFrameworkQueue, UnsupportedValidConfigurationsAreExplicit) {
  for (uint64_t Offset : {48, 56, 64, 72}) {
    queueConfiguration();
    put(QueueConfig + Offset, IoControlPC);
    expectError(createQueue(), "not modeled");
  }
  queueConfiguration();
  put(QueueConfig + 4, 2, 4);
  expectError(createQueue(), "bounded parallel queue delivery");
  queueConfiguration();
  put(QueueConfig + 4, 2, 4);
  put(QueueConfig + 80, 1, 4);
  expectError(createQueue(), "bounded parallel queue delivery");
  queueConfiguration();
  put(QueueConfig + 4, 3, 4);
  put(QueueConfig + 40, 0);
  expectError(createQueue(), "manual default queue");
  queueConfiguration();
  put(QueueConfig + 13, 0, 1);
  expectError(createQueue(), "nondefault automatic queue");
  for (uint64_t Size : {80, 88}) {
    queueConfiguration();
    put(QueueConfig, Size, 4);
    expectError(createQueue(), "legacy");
  }
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), 0u);
}

TEST_F(DriverKernelFrameworkQueue,
       ManualQueueHasItsOwnIdentityAndEmptyRetrievalClearsOutput) {
  put(QueueConfig + 4, framework::QueueDispatchManual, 4);
  put(QueueConfig + 13, 0, 1);
  put(QueueConfig + 40, 0);
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Manual = get(QueueSlot);
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), 0u);
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Manual})), Device);
  put(QueueSlot, Sentinel);
  EXPECT_EQ(take(invoke("WdfIoQueueRetrieveNextRequest",
                        {Globals, Manual, QueueSlot})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(QueueSlot), 0u);
  queueConfiguration();
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Default = get(QueueSlot);
  EXPECT_NE(Default, Manual);
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})),
            Default);
  expectError(
      invoke("WdfIoQueueRetrieveNextRequest", {Globals, Default, QueueSlot}),
      "sequential queue retrieval");
  take(invoke("WdfObjectDelete", {Globals, Manual}));
  EXPECT_TRUE(Released.count(Manual));
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})),
            Default);
}

TEST_F(DriverKernelFrameworkQueue,
       UnlimitedParallelDefaultQueueAcceptsThePublicConfiguration) {
  queueConfiguration();
  put(QueueConfig + 4, framework::QueueDispatchParallel, 4);
  put(QueueConfig + 80, UINT32_MAX, 4);
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Queue = get(QueueSlot);
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), Queue);
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Queue})), Device);
}

TEST_F(DriverKernelFrameworkQueue, PassivePolicyMustBeExplicitlyConfigured) {
  expectError(
      invoke("WdfIoQueueCreate", {Globals, Device, QueueConfig, 0, QueueSlot}),
      "explicit passive");
  attributes();
  expectError(createQueue(), "explicit passive");
  queueAttributes();
  EXPECT_EQ(take(createQueue()), 0u);
}

TEST_F(DriverKernelFrameworkQueue, ControlPowerTriStatesRemainNonPnp) {
  for (unsigned Power = 0; Power < 3; ++Power) {
    if (Power)
      Device = control();
    queueConfiguration();
    put(QueueConfig + 8, Power, 4);
    EXPECT_EQ(take(createQueue()), 0u);
  }
  Device = control();
  queueConfiguration();
  put(QueueConfig + 8, 3, 4);
  expectError(createQueue(), "tri-state");
}

TEST_F(DriverKernelFrameworkQueue,
       DuplicateAndInitializedDevicesRejectCreation) {
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Queue = get(QueueSlot);
  const size_t Attempts = AllocationAttempts;
  put(QueueSlot, Sentinel);
  EXPECT_EQ(take(createQueue()), 0xc0000001u);
  EXPECT_EQ(get(QueueSlot), Sentinel);
  EXPECT_EQ(AllocationAttempts, Attempts);
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), Queue);
  take(invoke("WdfControlFinishInitializing", {Globals, Device}));
  EXPECT_EQ(take(createQueue()), 0xc0000184u);
  EXPECT_EQ(AllocationAttempts, Attempts);
}

TEST_F(DriverKernelFrameworkQueue, QueueParentMustBelongToItsDevice) {
  queueAttributes(DriverHandle);
  EXPECT_EQ(take(createQueue()), 0xc0000010u);
  const auto OtherDevice = control();
  queueAttributes(OtherDevice);
  EXPECT_EQ(take(createQueue()), 0xc0000010u);
  attributes(Device);
  const auto Child = object(Attrs);
  queueAttributes(Child);
  expectError(createQueue(), "parenting below a device child");
  queueAttributes(Device);
  EXPECT_EQ(take(createQueue()), 0u);
}

TEST_F(DriverKernelFrameworkQueue, DeviceDeletionOwnsDefaultQueueTeardown) {
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Queue = get(QueueSlot);
  expectError(invoke("WdfObjectDelete", {Globals, Queue}), "default queue");
  take(invoke("WdfObjectDelete", {Globals, Device}));
  EXPECT_TRUE(HostDevices.empty());
  EXPECT_TRUE(Released.count(Queue));
  EXPECT_TRUE(Released.count(Device));
  expectError(invoke("WdfIoQueueGetDevice", {Globals, Queue}), "handle");
  expectError(invoke("WdfDeviceGetDefaultQueue", {Globals, Device}), "handle");
}

TEST_F(DriverKernelFrameworkQueue, QueueContextCleanupPrecedesDeviceCleanup) {
  Device = control(ParentCleanup, ParentDestroy);
  queueConfiguration();
  type();
  queueAttributes(0, Type, ChildCleanup, ChildDestroy);
  EXPECT_EQ(take(createQueue()), 0u);
  const auto Queue = get(QueueSlot);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Queue, Type}));
  ASSERT_NE(Context, 0u);
  take(invoke("WdfObjectDelete", {Globals, Device}));
  const auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Queue}));
  EXPECT_EQ(take(invoke("WdfIoQueueGetDevice", {Globals, Queue})), Device);
  finish(Call);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ParentCleanup, ChildDestroy,
                                            ParentDestroy}));
  expectError(Model.validateGuestAccess(Context, 1, false), "freed");
}

TEST_F(DriverKernelFrameworkQueue,
       WrongKindsAndInvalidOutputsCannotPublishQueue) {
  expectError(invoke("WdfDeviceGetDefaultQueue", {Globals, DriverHandle}),
              "wrong-kind");
  expectError(invoke("WdfIoQueueGetDevice", {Globals, Device}), "wrong-kind");
  put(QueueConfig + 88, Device);
  expectError(createQueue(), "queue driver");
  put(QueueConfig + 88, 0);
  DenyWriteAt = QueueSlot;
  const size_t Attempts = AllocationAttempts;
  expectError(createQueue(), "write validation");
  EXPECT_EQ(AllocationAttempts, Attempts);
  DenyWriteAt = 0;
  EXPECT_EQ(take(invoke("WdfDeviceGetDefaultQueue", {Globals, Device})), 0u);
}

} // namespace
} // namespace neverd::emulation

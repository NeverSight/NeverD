//===- KernelFrameworkQueueTestSupport.h - Shared queue fixture
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construct control devices and queues through the production framework APIs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_EMULATION_KERNELFRAMEWORKQUEUETESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_KERNELFRAMEWORKQUEUETESTSUPPORT_H

#include "KernelFrameworkTestSupport.h"

namespace neverd::emulation::framework_test {

class DriverKernelFrameworkQueue : public DriverKernelFramework {
protected:
  static constexpr uint64_t QueueConfig = Driver + 0x1000;
  static constexpr uint64_t Security = Driver + 0x1100;
  static constexpr uint64_t DeviceName = Driver + 0x1200;
  static constexpr uint64_t InitSlot = Driver + 0x1300;
  static constexpr uint64_t DeviceSlot = Driver + 0x1308;
  static constexpr uint64_t QueueSlot = Driver + 0x1310;
  static constexpr uint64_t IoControlPC = 0x180002000;
  std::set<uint64_t> HostDevices;
  uint64_t DriverHandle = 0;
  uint64_t Device = 0;
  uint64_t NextHostDevice = Driver + 0x8000;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Host;
    Host.Create =
        [this](llvm::StringRef Name,
               bool Direct) -> llvm::Expected<KernelFramework::DeviceCreation> {
      EXPECT_TRUE(Name.starts_with("\\Device\\QueueTest"));
      EXPECT_FALSE(Direct);
      const uint64_t Address = NextHostDevice;
      NextHostDevice += 0x100;
      HostDevices.insert(Address);
      return KernelFramework::DeviceCreation{0, Address};
    };
    Host.Delete = [this](uint64_t Address) -> llvm::Error {
      return HostDevices.erase(Address) == 1
                 ? llvm::Error::success()
                 : failure("host device deleted more than once");
    };
    Host.FinishInitializing = [this](uint64_t Address) -> llvm::Error {
      return HostDevices.count(Address)
                 ? llvm::Error::success()
                 : failure("unknown host device initialization");
    };
    Model.setDeviceHost(std::move(Host));
    bind();
    DriverHandle = createDriver();
    Device = control();
    queueConfiguration();
  }

  void string(uint64_t Descriptor, llvm::StringRef Text) {
    utf16(Descriptor + 32, Text);
    put(Descriptor, Text.size() * 2, 2);
    put(Descriptor + 2, Text.size() * 2 + 2, 2);
    put(Descriptor + 8, Descriptor + 32);
  }

  void queueAttributes(uint64_t Parent = 0, uint64_t TypeInfo = 0,
                       uint64_t Cleanup = 0, uint64_t Destroy = 0) {
    attributes(Parent, TypeInfo, Cleanup, Destroy);
    put(Attrs + 24, 2, 4);
    put(Attrs + 28, 4, 4);
  }

  uint64_t control(uint64_t Cleanup = 0, uint64_t Destroy = 0) {
    string(Security, "D:P(A;;GA;;;WD)");
    const auto Init = take(invoke("WdfControlDeviceInitAllocate",
                                  {Globals, DriverHandle, Security}));
    put(InitSlot, Init);
    string(DeviceName, "\\Device\\QueueTest" + std::to_string(NextHostDevice));
    EXPECT_EQ(
        take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName})),
        0u);
    queueAttributes(0, 0, Cleanup, Destroy);
    EXPECT_EQ(
        take(invoke("WdfDeviceCreate", {Globals, InitSlot, Attrs, DeviceSlot})),
        0u);
    EXPECT_EQ(get(InitSlot), 0u);
    return get(DeviceSlot);
  }

  void queueConfiguration() {
    success(Memory.write(QueueConfig, std::vector<uint8_t>(96)));
    put(QueueConfig, 96, 4);
    put(QueueConfig + 4, 1, 4);
    put(QueueConfig + 8, 2, 4);
    put(QueueConfig + 13, 1, 1);
    put(QueueConfig + 40, IoControlPC);
    queueAttributes();
    put(QueueSlot, Sentinel);
  }

  llvm::Expected<uint64_t> createQueue(uint64_t OutputSlot = QueueSlot) {
    return invoke("WdfIoQueueCreate",
                  {Globals, Device, QueueConfig, Attrs, OutputSlot});
  }
};

} // namespace neverd::emulation::framework_test

#endif

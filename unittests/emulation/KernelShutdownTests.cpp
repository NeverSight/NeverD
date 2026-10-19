//===- KernelShutdownTests.cpp - Shutdown registration ownership ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shutdown registration ownership.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
class KernelShutdown : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x22000000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Device = 0;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    auto R = Model->call(Name, Args);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void error(const char *Name, std::initializer_list<uint64_t> Args,
             llvm::StringRef Text) {
    auto R = Model->call(Name, Args);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find(Text.str()),
              std::string::npos);
  }
  uint64_t read(uint64_t A, unsigned Width = 8) {
    auto R = Memory->readInteger(A, Width);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint32_t flags() {
    return uint32_t(read(Device + windows::DeviceFlagsOffset, 4));
  }
  uint64_t device() {
    EXPECT_EQ(call("IoCreateDevice",
                   {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch}),
              0u);
    return read(Scratch);
  }
  void SetUp() override {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
    Memory = std::move(*B);
    ok(Memory->map(Scratch, 4096, Read | Write));
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterExecution(profile::StackBase);
    Device = device();
  }
};

TEST_F(KernelShutdown, RegistrationSetsOnlyItsFlagAndDoesNotDispatch) {
  const auto Before = flags();
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(flags(), Before | 0x800u);
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Device);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(flags(), Before);
}

TEST_F(KernelShutdown, UnregisterCancelsAllDuplicateEntriesAndReleasesBudget) {
  for (unsigned I = 0; I != 64; ++I)
    EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0xc000009au);
  EXPECT_NE(flags() & 0x800u, 0u);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(flags() & 0x800u, 0u);
  for (unsigned I = 0; I != 64; ++I)
    EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
}

TEST_F(KernelShutdown, DeviceQueuesAreIndependentAndShareOneBoundedBudget) {
  const auto Other = device();
  for (unsigned I = 0; I != 63; ++I)
    EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Other}), 0u);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Other}), 0xc000009au);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
  EXPECT_NE(read(Other + windows::DeviceFlagsOffset, 4) & 0x800u, 0u);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Other}), 0u);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Other}), 0u);
}

TEST_F(KernelShutdown, RejectsUnknownDeletedAndNeverRegisteredObjects) {
  error("IoRegisterShutdownNotification", {Scratch}, "live guest-owned");
  error("IoRegisterShutdownNotification", {0}, "live guest-owned");
  error("IoUnregisterShutdownNotification", {Device}, "prior registration");
  EXPECT_EQ(call("IoDeleteDevice", {Device}), 0u);
  error("IoRegisterShutdownNotification", {Device}, "live guest-owned");
  error("IoUnregisterShutdownNotification", {Device}, "live guest-owned");
}

TEST_F(KernelShutdown, PassiveAPIGatesPreserveTheQueueOnFailure) {
  const auto Before = flags();
  const auto Previous = call("KfRaiseIrql", {1});
  error("IoRegisterShutdownNotification", {Device}, "IRQL");
  EXPECT_EQ(flags(), Before);
  EXPECT_EQ(call("KeLowerIrql", {Previous}), 0u);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  error("IoUnregisterShutdownNotification", {Device}, "IRQL");
  EXPECT_NE(flags() & 0x800u, 0u);
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
}

TEST_F(KernelShutdown, DeleteCancelsDuplicatesBeforeRetiringTheDevice) {
  for (unsigned I = 0; I != 64; ++I)
    EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  // IoDeleteDevice itself admits APC_LEVEL. Its internal cancellation is not
  // a new guest call to the PASSIVE_LEVEL-only unregister entry point.
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  EXPECT_EQ(call("IoDeleteDevice", {Device}), 0u);
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  const auto New = device();
  EXPECT_NE(New, Device);
  EXPECT_EQ(call("IoRegisterShutdownNotification", {New}), 0u);
}

TEST_F(KernelShutdown, CorruptFlagFailsBeforeChangingRegistrationState) {
  EXPECT_EQ(call("IoRegisterShutdownNotification", {Device}), 0u);
  const auto Registered = flags();
  ok(Memory->writeInteger(Device + windows::DeviceFlagsOffset,
                          Registered & ~0x800u, 4));
  error("IoDeleteDevice", {Device}, "shutdown registration flag");
  error("IoRegisterShutdownNotification", {Device},
        "shutdown registration flag");
  ok(Memory->writeInteger(Device + windows::DeviceFlagsOffset, Registered, 4));
  EXPECT_EQ(call("IoUnregisterShutdownNotification", {Device}), 0u);
}

TEST(KernelShutdownDriver, CompiledRegistrationAndDeleteUnwindWithoutShutdown) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_shutdown_registration.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      EXPECT_TRUE(R->Devices.empty());
    }
}
} // namespace
} // namespace neverd::emulation

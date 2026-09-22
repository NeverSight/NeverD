//===- KernelResourceAssignmentTests.cpp - Shared physical assignments --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify packed raw/translated memory and interrupt lists, physical epochs and
/// release guards independently of MMIO mappings and interrupt consumers.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelResources.h"

namespace neverd::emulation {
namespace {
class KernelResourceAssignmentTest : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x2000, OtherPDO = 0x3000;
  static constexpr uint32_t Failure = 0xc0000001;
  bool Blocked = false;
  std::vector<uint64_t> Checked;
  KernelResources Model{[this](uint64_t Owner) -> llvm::Error {
    Checked.push_back(Owner);
    if (Blocked)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                      "live resource consumer");
    return llvm::Error::success();
  }};

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
  DriverPnpDevice configuration(unsigned Index = 0) {
    DriverPnpDevice Device;
    Device.ID = "resources" + std::to_string(Index);
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.Resources.push_back({"bar0", 0x123400000ULL + Index * 0x10000,
                                0x234500000ULL + Index * 0x10000, 0x1000,
                                {{0, 4, DriverRegisterAccess::ReadWrite, 9}}});
    for (unsigned I = 0; I != 2; ++I) {
      DriverInterruptResource Interrupt;
      Interrupt.ID = "irq" + std::to_string(I);
      Interrupt.RawVector = 17 + I + Index * 2;
      Interrupt.RawLevel = 0xabcd;
      Interrupt.RawAffinity = 1;
      Interrupt.TranslatedVector = 0x91 + I + Index * 2;
      Interrupt.TranslatedLevel = 5 + I;
      Interrupt.TranslatedAffinity = 1;
      Device.Interrupts.push_back(Interrupt);
    }
    return Device;
  }
  void start(uint64_t Owner = PDO) {
    ok(Model.beginStart(Owner));
    ok(Model.completeLowerStart(Owner, 0));
    ok(Model.finishPnp(Owner, DevicePnpRequest::Start, 0));
  }
  static uint64_t read(const std::vector<uint8_t> &Bytes, size_t Offset,
                       unsigned Width) {
    uint64_t Value = 0;
    for (unsigned I = 0; I != Width; ++I)
      Value |= uint64_t(Bytes.at(Offset + I)) << (8 * I);
    return Value;
  }
  void SetUp() override { ok(Model.configure(PDO, configuration())); }
};

TEST_F(KernelResourceAssignmentTest, MemoryThenInterruptsUseGenuinePackedAbi) {
  const auto Raw = take(Model.resourceList(PDO, false));
  const auto Translated = take(Model.resourceList(PDO, true));
  ASSERT_EQ(Raw.size(), 80u);
  ASSERT_EQ(Translated.size(), 80u);
  for (const auto *Bytes : {&Raw, &Translated}) {
    EXPECT_EQ(read(*Bytes, 0, 4), 1u);
    EXPECT_EQ(read(*Bytes, 4, 4), 0u);
    EXPECT_EQ(read(*Bytes, 8, 4), 0u);
    EXPECT_EQ(read(*Bytes, 12, 2), 1u);
    EXPECT_EQ(read(*Bytes, 14, 2), 1u);
    EXPECT_EQ(read(*Bytes, 16, 4), 3u);
    EXPECT_EQ(read(*Bytes, 20, 1), 3u);
    EXPECT_EQ(read(*Bytes, 21, 1), 1u);
    EXPECT_EQ(read(*Bytes, 22, 2), 0u);
    EXPECT_EQ(read(*Bytes, 32, 4), 0x1000u);
    for (unsigned I = 0; I != 2; ++I) {
      const size_t Base = 40 + I * 20;
      EXPECT_EQ(read(*Bytes, Base, 1), 2u);
      EXPECT_EQ(read(*Bytes, Base + 1, 1), 1u);
      EXPECT_EQ(read(*Bytes, Base + 2, 2), 1u);
      EXPECT_EQ(read(*Bytes, Base + 6, 2), 0u);
      EXPECT_EQ(read(*Bytes, Base + 12, 8), 1u);
    }
  }
  EXPECT_EQ(read(Raw, 24, 8), 0x123400000ULL);
  EXPECT_EQ(read(Translated, 24, 8), 0x234500000ULL);
  EXPECT_EQ(read(Raw, 44, 4), 0xabcdu);
  EXPECT_EQ(read(Raw, 48, 4), 17u);
  EXPECT_EQ(read(Translated, 44, 4), 5u);
  EXPECT_EQ(read(Translated, 48, 4), 0x91u);
  EXPECT_EQ(read(Translated, 64, 4), 6u);
  EXPECT_EQ(read(Translated, 68, 4), 0x92u);
}

TEST_F(KernelResourceAssignmentTest, StartPublishesOneEpochOnlyAtLowerCompletion) {
  EXPECT_FALSE(Model.find(PDO)->Assigned);
  EXPECT_EQ(Model.find(PDO)->Epoch, 0u);
  ok(Model.beginStart(PDO));
  EXPECT_TRUE(Model.find(PDO)->Starting);
  EXPECT_EQ(Model.find(PDO)->Epoch, 1u);
  EXPECT_FALSE(Model.find(PDO)->Assigned);
  reject(Model.beginStart(PDO), "unassigned");
  reject(Model.validateCompletion(PDO, DevicePnpRequest::Start, 0), "actual");
  ok(Model.completeLowerStart(PDO, 0));
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  EXPECT_TRUE(Model.find(PDO)->Starting);
  reject(Model.completeLowerStart(PDO, 0), "pending");
  ok(Model.finishPnp(PDO, DevicePnpRequest::Start, 0));
  EXPECT_FALSE(Model.find(PDO)->Starting);
  EXPECT_EQ(Model.find(PDO)->Epoch, 1u);
  EXPECT_EQ(Model.find(PDO)->Memory.size(), 1u);
  EXPECT_EQ(Model.find(PDO)->Interrupts.size(), 2u);
}

TEST_F(KernelResourceAssignmentTest, ReleaseGuardRejectsBeforeAnyStateMutation) {
  Blocked = true;
  reject(Model.beginStart(PDO), "consumer");
  EXPECT_EQ(Model.find(PDO)->Epoch, 0u);
  EXPECT_FALSE(Model.find(PDO)->Starting);
  Blocked = false;
  start();
  Blocked = true;
  reject(Model.finishPnp(PDO, DevicePnpRequest::Stop, 0), "consumer");
  reject(Model.finishPnp(PDO, DevicePnpRequest::Remove, 0), "consumer");
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  EXPECT_TRUE(Model.find(PDO)->Present);
  EXPECT_FALSE(Model.find(PDO)->Starting);
  EXPECT_EQ(Model.find(PDO)->Epoch, 1u);
  Blocked = false;
  ok(Model.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  start();
  EXPECT_EQ(Model.find(PDO)->Epoch, 2u);
  EXPECT_EQ(Checked.back(), PDO);
}

TEST_F(KernelResourceAssignmentTest, FailedLowerAndUpperStartsRetainRetryFacts) {
  const auto Before = take(Model.resourceList(PDO, true));
  ok(Model.beginStart(PDO));
  ok(Model.completeLowerStart(PDO, Failure));
  EXPECT_FALSE(Model.find(PDO)->Assigned);
  ok(Model.finishPnp(PDO, DevicePnpRequest::Start, Failure));
  EXPECT_FALSE(Model.find(PDO)->Starting);
  EXPECT_EQ(Model.find(PDO)->Epoch, 1u);
  ok(Model.beginStart(PDO));
  ok(Model.completeLowerStart(PDO, 0));
  Blocked = true;
  reject(Model.finishPnp(PDO, DevicePnpRequest::Start, Failure), "consumer");
  EXPECT_TRUE(Model.find(PDO)->Starting);
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  EXPECT_EQ(Model.find(PDO)->Epoch, 2u);
  Blocked = false;
  ok(Model.finishPnp(PDO, DevicePnpRequest::Start, Failure));
  start();
  EXPECT_EQ(Model.find(PDO)->Epoch, 3u);
  EXPECT_EQ(take(Model.resourceList(PDO, true)), Before);
}

TEST_F(KernelResourceAssignmentTest, PhysicalPowerAndPresenceAreIndependentOfEpoch) {
  start();
  const auto Epoch = Model.find(PDO)->Epoch;
  Model.setPhysicalPower(PDO, DevicePowerState::D3);
  EXPECT_EQ(Model.find(PDO)->Power, DevicePowerState::D3);
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  EXPECT_EQ(Model.find(PDO)->Epoch, Epoch);
  Model.setPhysicalPower(PDO, DevicePowerState::D0);
  EXPECT_EQ(Model.find(PDO)->Power, DevicePowerState::D0);
  Model.surpriseRemoval(PDO);
  EXPECT_FALSE(Model.find(PDO)->Present);
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  EXPECT_EQ(Model.find(PDO)->Epoch, Epoch);
  reject(Model.beginStart(PDO), "present");
  Blocked = true;
  reject(Model.finishPnp(PDO, DevicePnpRequest::Remove, 0), "consumer");
  EXPECT_TRUE(Model.find(PDO)->Assigned);
  Blocked = false;
  ok(Model.finishPnp(PDO, DevicePnpRequest::Remove, 0));
  EXPECT_FALSE(Model.find(PDO)->Assigned);
  EXPECT_FALSE(Model.find(PDO)->Present);
}

TEST_F(KernelResourceAssignmentTest, MultipleProvidersHaveIndependentStateAndLists) {
  ok(Model.configure(OtherPDO, configuration(1)));
  const auto OtherBefore = take(Model.resourceList(OtherPDO, true));
  start();
  EXPECT_FALSE(Model.find(OtherPDO)->Assigned);
  start(OtherPDO);
  Model.setPhysicalPower(PDO, DevicePowerState::D3);
  ok(Model.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  start();
  EXPECT_EQ(Model.find(PDO)->Epoch, 2u);
  EXPECT_EQ(Model.find(OtherPDO)->Epoch, 1u);
  EXPECT_EQ(Model.find(OtherPDO)->Power, DevicePowerState::D0);
  EXPECT_TRUE(Model.find(OtherPDO)->Assigned);
  EXPECT_EQ(take(Model.resourceList(OtherPDO, true)), OtherBefore);
  reject(Model.configure(OtherPDO, configuration(1)), "duplicate");
  EXPECT_EQ(take(Model.resourceList(OtherPDO, true)), OtherBefore);
}
} // namespace
} // namespace neverd::emulation

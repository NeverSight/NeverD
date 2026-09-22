//===- KernelMMIOTests.cpp - Physical banks and mapping lifetime tests
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify independent resource epochs, physical register values, aliases and
/// exact mapped ranges through the real backend memory boundary.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/KernelMMIO.h"

namespace neverd::emulation {
namespace {
class KernelMMIOTest : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x1230, Physical = 0xf0000004;
  static constexpr uint32_t Failure = 0xc0000001;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelMMIO> Model;
  KernelResources Resources{[this](uint64_t PDO) { return Model->canRemove(PDO); }};
  DriverPnpDevice Device;

  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const std::string Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    reject(Value.takeError(), Text);
  }
  void start(uint64_t Owner = PDO) {
    ok(Resources.beginStart(Owner));
    ok(Resources.completeLowerStart(Owner, 0));
    ok(Resources.finishPnp(Owner, DevicePnpRequest::Start, 0));
  }
  uint64_t map(uint64_t PA = Physical, uint64_t Length = 0x1000) {
    return take(Model->map(PA, Length, mmio::NonCached, false));
  }
  uint64_t get(uint64_t Address, unsigned Size = 4) {
    return take(Memory->readInteger(Address, Size));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(16 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    Model = std::make_unique<KernelMMIO>(*Memory, Resources);
    Device.ID = "bank0";
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.Resources.push_back(
        {"bar0",
         0x4004,
         Physical,
         0x1000,
         {{0, 4, DriverRegisterAccess::ReadWrite, 17},
          {4, 4, DriverRegisterAccess::ReadOnly, 23},
          {8, 1, DriverRegisterAccess::ReadWrite, 42},
          {10, 2, DriverRegisterAccess::ReadWrite, 0x4321},
          {0xffc, 4, DriverRegisterAccess::ReadWrite, 99}}});
    ok(Resources.configure(PDO, Device));
    ok(Model->configure(PDO));
  }
};

TEST_F(KernelMMIOTest, ResourceListsUsePackedOrderedRawAndTranslatedFacts) {
  auto Read = [](const std::vector<uint8_t> &Bytes, size_t Offset,
                 unsigned Width) {
    uint64_t Value = 0;
    for (unsigned I = 0; I < Width; ++I)
      Value |= uint64_t(Bytes.at(Offset + I)) << (I * 8);
    return Value;
  };
  auto Raw = take(Resources.resourceList(PDO, false));
  auto Translated = take(Resources.resourceList(PDO, true));
  ASSERT_EQ(Raw.size(), 40u);
  ASSERT_EQ(Translated.size(), 40u);
  EXPECT_EQ(Read(Raw, 0, 4), 1u);
  EXPECT_EQ(Read(Raw, 4, 4), 0u);
  EXPECT_EQ(Read(Raw, 8, 4), 0u);
  EXPECT_EQ(Read(Raw, 12, 2), 1u);
  EXPECT_EQ(Read(Raw, 14, 2), 1u);
  EXPECT_EQ(Read(Raw, 16, 4), 1u);
  EXPECT_EQ(Read(Raw, 20, 1), 3u);
  EXPECT_EQ(Read(Raw, 21, 1), 1u);
  EXPECT_EQ(Read(Raw, 24, 8), 0x4004u);
  EXPECT_EQ(Read(Translated, 24, 8), Physical);
  EXPECT_EQ(Read(Raw, 32, 4), 0x1000u);
}

TEST_F(KernelMMIOTest, LowerStartControlsAssignmentBeforeTopCompletion) {
  reject(Model->map(Physical, 4, 0, false), "not assigned");
  ok(Resources.beginStart(PDO));
  reject(Model->map(Physical, 4, 0, false), "not assigned");
  reject(Resources.validateCompletion(PDO, DevicePnpRequest::Start, 0), "epoch");
  ok(Resources.completeLowerStart(PDO, 0));
  const uint64_t Alias = map();
  EXPECT_EQ(get(Alias), 17u);
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Start, 0));
  ok(Model->unmap(Alias, 0x1000));
}

TEST_F(KernelMMIOTest, FailedLowerAndUpperStartsRetainTruthfulRegisterEffects) {
  ok(Resources.beginStart(PDO));
  ok(Resources.completeLowerStart(PDO, Failure));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Start, Failure));
  reject(Model->map(Physical, 4, 0, false), "not assigned");
  ok(Resources.beginStart(PDO));
  ok(Resources.completeLowerStart(PDO, 0));
  const uint64_t Alias = map();
  ok(Memory->writeInteger(Alias, 55, 4));
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Start, Failure), "mapping");
  ok(Model->unmap(Alias, 0x1000));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Start, Failure));
  start();
  EXPECT_EQ(get(map()), 55u);
}

TEST_F(KernelMMIOTest, AliasesShareValuesAndExactUnmapPreservesOtherAliases) {
  start();
  const auto First = map(), Second = map(Physical, 4);
  EXPECT_EQ(First % 4096, Physical % 4096);
  EXPECT_NE(First, Second);
  ok(Memory->writeInteger(First, 0x55667788, 4));
  EXPECT_EQ(get(Second), 0x55667788u);
  reject(Model->unmap(First, 4), "original");
  EXPECT_EQ(get(First), 0x55667788u);
  ok(Model->unmap(First, 0x1000));
  EXPECT_EQ(get(Second), 0x55667788u);
  ok(Model->unmap(Second, 4));
  const uint64_t NewAlias = map();
  EXPECT_GT(NewAlias, Second);
  EXPECT_EQ(get(NewAlias), 0x55667788u);
}

TEST_F(KernelMMIOTest, StopRequiresUnmapAndRestartPreservesBankState) {
  start();
  const auto First = map();
  ok(Memory->writeInteger(First, 77, 4));
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0), "mapping");
  EXPECT_EQ(get(First), 77u);
  ok(Model->unmap(First, 0x1000));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  reject(Model->map(Physical, 4, 0, false), "not assigned");
  start();
  EXPECT_EQ(get(map()), 77u);
}

TEST_F(KernelMMIOTest, PhysicalPowerChangesDoNotResetOrUnmapRegisters) {
  start();
  const auto Alias = map();
  ok(Memory->writeInteger(Alias, 88, 4));
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  // Mapping changes do not perform device register accesses.
  const auto Other = map(Physical, 4);
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  EXPECT_EQ(get(Other), 88u);
  EXPECT_EQ(get(Alias), 88u);
}

TEST_F(KernelMMIOTest, PhysicalD3RejectsRegisterAccess) {
  start();
  const auto Alias = map();
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  reject(Memory->readInteger(Alias, 4), "D0");
}

TEST_F(KernelMMIOTest, SurpriseRemovalPermitsCleanupButNoNewMappings) {
  start();
  const auto Alias = map();
  Resources.surpriseRemoval(PDO);
  reject(Model->map(Physical, 4, 0, false), "hardware is absent");
  reject(Model->canRemove(PDO), "mapping");
  ok(Model->unmap(Alias, 0x1000));
  ok(Model->canRemove(PDO));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Remove, 0));
  reject(Resources.beginStart(PDO), "present");
}

TEST_F(KernelMMIOTest, SurpriseRemovalRejectsAlreadyMappedAccess) {
  start();
  const auto Alias = map();
  Resources.surpriseRemoval(PDO);
  reject(Memory->readInteger(Alias, 4), "unavailable");
}

TEST_F(KernelMMIOTest, ReadOnlyAliasesCannotGrantRegisterWrite) {
  start();
  const auto Alias = take(
      Model->map(Physical, 4, mmio::PageNoCache | mmio::PageReadOnly, true));
  EXPECT_EQ(get(Alias), 17u);
  reject(Memory->writeInteger(Alias, 9, 4), "read-only");
}

TEST_F(KernelMMIOTest, ReadOnlyRegistersRemainReadOnlyInWriteableMappings) {
  start();
  const auto Alias = map();
  EXPECT_EQ(get(Alias + 4), 23u);
  reject(Memory->writeInteger(Alias + 4, 9, 4), "read-only");
}

TEST_F(KernelMMIOTest, PagePaddingDoesNotExposeAdjacentRegisters) {
  start();
  const auto Alias = map(Physical + 4, 4);
  EXPECT_EQ(get(Alias), 23u);
  reject(Memory->readInteger(Alias - 4, 4), "exact mapped");
}

TEST_F(KernelMMIOTest, LastResourceRegisterSurvivesCrossPageMapping) {
  start();
  const auto Alias = map(Physical + 0xffc, 4);
  EXPECT_EQ(get(Alias), 99u);
  ok(Memory->writeInteger(Alias, 101, 4));
  ok(Model->unmap(Alias, 4));
  EXPECT_EQ(get(map() + 0xffc), 101u);
}

TEST_F(KernelMMIOTest, UnknownRawPhysicalAndCachePoliciesDoNotCreateMappings) {
  start();
  reject(Model->map(0x4004, 4, 0, false), "translated resource");
  reject(Model->map(Physical, 0x1001, 0, false), "translated resource");
  reject(Model->map(UINT64_MAX, 2, 0, false), "overflowing");
  reject(Model->map(Physical, 0, 0, false), "invalid");
  reject(Model->map(Physical, 4, 1, false), "noncached");
  reject(Model->map(Physical, 4, 4, true), "noncached");
  ok(Model->canRemove(PDO));
  EXPECT_EQ(get(map()), 17u);
}

TEST_F(KernelMMIOTest,
       MappingCapacityReturnsNullWithoutChangingExistingAliases) {
  start();
  std::vector<uint64_t> Aliases;
  for (unsigned I = 0; I < mmio::MaxMappings; ++I)
    Aliases.push_back(map(Physical, 4));
  EXPECT_EQ(map(Physical, 4), 0u);
  EXPECT_EQ(get(Aliases.front()), 17u);
  ok(Model->unmap(Aliases.back(), 4));
  EXPECT_GT(map(Physical, 4), Aliases.back());
}

TEST_F(KernelMMIOTest, IndependentPDOsDoNotShareEpochsOrValues) {
  Device.Resources[0].TranslatedStart += 0x10000;
  ok(Resources.configure(PDO + 8, Device));
  ok(Model->configure(PDO + 8));
  start();
  start(PDO + 8);
  const auto First = map(), Second = map(Physical + 0x10000);
  ok(Memory->writeInteger(First, 61, 4));
  EXPECT_EQ(get(Second), 17u);
  Resources.surpriseRemoval(PDO);
  EXPECT_EQ(get(Second), 17u);
  ok(Model->unmap(First, 0x1000));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Remove, 0));
  EXPECT_EQ(get(Second), 17u);
}
TEST_F(KernelMMIOTest, MappingCallbacksCannotOutliveTheirRegisterBankOwner) {
  start();
  const auto Alias = map();
  Model.reset();
  reject(Memory->readInteger(Alias, 4), "owner no longer exists");
}

TEST_F(KernelMMIOTest, BackendMappingCapacityReturnsNullAndCanRecover) {
  auto Limited = take(UnicornBackend::create(3 * 4096));
  ASSERT_TRUE(Limited);
  std::unique_ptr<KernelMMIO> Bank;
  KernelResources Assignment{[&](uint64_t Owner) { return Bank->canRemove(Owner); }};
  Bank = std::make_unique<KernelMMIO>(*Limited, Assignment);
  ok(Assignment.configure(PDO, Device));
  ok(Bank->configure(PDO));
  ok(Assignment.beginStart(PDO));
  ok(Assignment.completeLowerStart(PDO, 0));
  ok(Assignment.finishPnp(PDO, DevicePnpRequest::Start, 0));
  const auto First = take(Bank->map(Physical, 0x1000, 0, false));
  ASSERT_NE(First, 0u);
  EXPECT_EQ(take(Bank->map(Physical, 0x1000, 0, false)), 0u);
  EXPECT_FALSE(Limited->hasDeviceError());
  EXPECT_FALSE(Limited->fault());
  EXPECT_EQ(take(Limited->readInteger(First, 4)), 17u);
  ok(Bank->unmap(First, 0x1000));
  const auto Second = take(Bank->map(Physical, 0x1000, 0, false));
  EXPECT_GT(Second, First);
  EXPECT_EQ(take(Limited->readInteger(Second, 4)), 17u);
}

} // namespace
} // namespace neverd::emulation

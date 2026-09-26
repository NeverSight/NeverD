//===- BackendBackingTests.cpp - Device access to canonical guest RAM -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Backing access bypasses CPU permission checks without changing them, and
/// rejects whole invalid spans before RAM or MMIO effects.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"

#include "neverd/emulation/DriverProfile.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x1000;
constexpr uint64_t Data = 0x4000;
constexpr uint64_t Page = profile::PageSize;

class DriverBackendBacking : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;
  void SetUp() override {
    auto Result = UnicornBackend::create(5 * Page);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    CPU = std::move(*Result);
    ASSERT_EQ(llvm::toString(CPU->map(Code, Page, Read | Write | Execute)), "");
    ASSERT_EQ(llvm::toString(CPU->map(Data, Page, 0)), "");
  }
  void protectedAccess(bool IsWrite) {
    std::array<uint8_t, 8> Bytes{0xa5, 0x19, 2, 3, 4, 5, 6, 7}, ReadBack{};
    ASSERT_EQ(llvm::toString(CPU->writeBacking(Data, Bytes)), "");
    ASSERT_EQ(llvm::toString(CPU->readBacking(Data, ReadBack)), "");
    EXPECT_EQ(ReadBack, Bytes);
    const std::array<uint8_t, 3> CodeBytes{
        0x48, static_cast<uint8_t>(IsWrite ? 0x89 : 0x8b), 0x01};
    ASSERT_EQ(llvm::toString(CPU->write(Code, CodeBytes)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Data)), "");
    EXPECT_NE(llvm::toString(CPU->run(Code, 1000000)), "");
    ASSERT_TRUE(CPU->fault());
    EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(CPU->fault()->Address, Data);
    EXPECT_EQ(CPU->fault()->Access,
              IsWrite ? BackendAccessKind::Write : BackendAccessKind::Read);
  }
};

TEST_F(DriverBackendBacking, DeviceReadWriteDoesNotGrantCPUReadPermission) {
  protectedAccess(false);
}
TEST_F(DriverBackendBacking, DeviceReadWriteDoesNotGrantCPUWritePermission) {
  protectedAccess(true);
}

TEST_F(DriverBackendBacking, SharesBytesWithCPUAndAcrossProtectedPages) {
  ASSERT_EQ(llvm::toString(CPU->map(Data + Page, Page, 0)), "");
  ASSERT_EQ(llvm::toString(CPU->protect(Data, Page, Read | Write)), "");
  const std::array<uint8_t, 4> Original{1, 2, 3, 4}, Updated{9, 8, 7, 6};
  ASSERT_EQ(llvm::toString(CPU->write(Data, Original)), "");
  std::array<uint8_t, 4> Bytes{};
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data, Bytes)), "");
  EXPECT_EQ(Bytes, Original);
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data, Updated)), "");
  ASSERT_EQ(llvm::toString(CPU->read(Data, Bytes)), "");
  EXPECT_EQ(Bytes, Updated);
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data + Page - 2, Original)), "");
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data + Page - 2, Bytes)), "");
  EXPECT_EQ(Bytes, Original);
}

TEST_F(DriverBackendBacking, SharedVirtualAliasHasOneRAMAuthority) {
  constexpr uint64_t Alias = 0x8000;
  ASSERT_EQ(llvm::toString(CPU->protect(Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data + 5, 0x11223344, 4)), "");
  EXPECT_EQ(*CPU->readInteger(Alias + 5, 4), 0x11223344u);
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias + 5, 0xaabbccdd, 4)), "");
  EXPECT_EQ(*CPU->readInteger(Data + 5, 4), 0xaabbccddu);
  EXPECT_NE(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read)), "");
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendBacking, AliasRetirementPreservesBackingAndDerivedAliases) {
  constexpr uint64_t Alias = 0x8000, Derived = 0xa000;
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Derived, Alias, Page, Read)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias, 0x12345678, 4)), "");
  ASSERT_EQ(llvm::toString(CPU->unmapAlias(Alias, Page)), "");
  auto Accessible = CPU->canAccess(Alias, 1, Read);
  ASSERT_TRUE(bool(Accessible)) << llvm::toString(Accessible.takeError());
  EXPECT_FALSE(*Accessible);
  EXPECT_EQ(*CPU->readInteger(Derived, 4), 0x12345678u);
  EXPECT_EQ(llvm::toString(CPU->validateBacking(Data, Page)), "");

  ASSERT_EQ(llvm::toString(CPU->map(Alias, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias, 0xaabbccdd, 4)), "");
  EXPECT_EQ(*CPU->readInteger(Derived, 4), 0x12345678u);
  EXPECT_NE(llvm::toString(CPU->unmapAlias(Alias, Page)), "");
  EXPECT_NE(llvm::toString(CPU->unmapAlias(Data, Page)), "");
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendBacking,
       AliasUnmapRequiresExactExtentDespiteProtectionSplits) {
  constexpr uint64_t Source = 0x10000, Alias = 0x20000;
  auto Created = UnicornBackend::create(4 * Page);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto &Backend = **Created;
  ASSERT_EQ(llvm::toString(Backend.map(Source, 2 * Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Backend.mapAlias(Alias, Source, 2 * Page, Read)),
            "");
  ASSERT_EQ(llvm::toString(Backend.protect(Alias + Page, Page, 0)), "");
  for (auto [Address, Size] :
       std::array<std::pair<uint64_t, uint64_t>, 5>{{{Alias, 0},
                                                     {Alias, Page},
                                                     {Alias + Page, Page},
                                                     {Alias, 3 * Page},
                                                     {Source, 2 * Page}}}) {
    EXPECT_NE(llvm::toString(Backend.unmapAlias(Address, Size)), "");
    EXPECT_EQ(llvm::toString(Backend.validateBacking(Alias, 2 * Page)), "");
  }
  EXPECT_FALSE(Backend.fault());
  ASSERT_EQ(llvm::toString(Backend.unmapAlias(Alias, 2 * Page)), "");
  EXPECT_NE(llvm::toString(Backend.unmapAlias(Alias, 2 * Page)), "");
  // Repeated retirement returns the exact mapping budget, not the RAM owner.
  for (unsigned Iteration = 0; Iteration != 8; ++Iteration) {
    ASSERT_EQ(llvm::toString(Backend.mapAlias(Alias, Source, 2 * Page, Read)),
              "");
    ASSERT_EQ(llvm::toString(Backend.unmapAlias(Alias, 2 * Page)), "");
  }
  EXPECT_EQ(llvm::toString(Backend.validateBacking(Source, 2 * Page)), "");
}

TEST_F(DriverBackendBacking, ReboundAliasUsesNewBackingInResumedCPUContexts) {
  constexpr uint64_t Alias = 0x8000, Replacement = 0xa000;
  ASSERT_EQ(llvm::toString(CPU->map(Replacement, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Replacement, 0xaabbccdd, 4)), "");
  const std::array<uint8_t, 4> Original{0x44, 0x33, 0x22, 0x11};
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data, Original)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read)), "");
  ASSERT_EQ(llvm::toString(CPU->write(Code, {0x8b, 0x01, 0x90})), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Alias)), "");
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Code + 2)
      CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  auto Context = CPU->saveContext();
  ASSERT_TRUE(bool(Context)) << llvm::toString(Context.takeError());
  ASSERT_EQ(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), 0x11223344u);
  ASSERT_EQ(llvm::toString(CPU->unmapAlias(Alias, Page)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Replacement, Page, Read)), "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Context)), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), 0xaabbccddu);
  std::array<uint8_t, 4> Bytes{};
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data, Bytes)), "");
  EXPECT_EQ(Bytes, Original);
}

TEST_F(DriverBackendBacking, AliasReplacementCanMergeAndSplitAtFullBudget) {
  constexpr uint64_t First = 0x10000, Second = 0x20000, Alias = 0x30000;
  auto Created = UnicornBackend::create(6 * Page);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto &Backend = **Created;
  ASSERT_EQ(llvm::toString(Backend.map(First, 2 * Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Backend.map(Second, 2 * Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Backend.writeInteger(First, 0x11, 1)), "");
  ASSERT_EQ(llvm::toString(Backend.writeInteger(Second, 0x22, 1)), "");
  ASSERT_EQ(llvm::toString(Backend.writeInteger(Second + Page, 0x33, 1)), "");
  ASSERT_EQ(llvm::toString(Backend.replaceAliases(
                {}, {{Alias, First, Page, Read | Write},
                     {Alias + Page, First + Page, Page, Read | Write}})),
            "");
  ASSERT_EQ(llvm::toString(
                Backend.replaceAliases({{Alias, Page}, {Alias + Page, Page}},
                                       {{Alias, Second, 2 * Page, Read}})),
            "");
  EXPECT_EQ(*Backend.readInteger(Alias, 1), 0x22u);
  EXPECT_EQ(*Backend.readInteger(Alias + Page, 1), 0x33u);
  auto Writable = Backend.canAccess(Alias, 2 * Page, Write);
  ASSERT_TRUE(bool(Writable)) << llvm::toString(Writable.takeError());
  EXPECT_FALSE(*Writable);
  ASSERT_EQ(llvm::toString(Backend.replaceAliases(
                {{Alias, 2 * Page}}, {{Alias, First, Page, Read},
                                      {Alias + Page, Second, Page, Read}})),
            "");
  EXPECT_EQ(*Backend.readInteger(Alias, 1), 0x11u);
  EXPECT_EQ(*Backend.readInteger(Alias + Page, 1), 0x22u);
  EXPECT_FALSE(Backend.fault());
}

TEST_F(DriverBackendBacking, ReplacementPreflightPreservesEveryAliasOnFailure) {
  constexpr uint64_t Alias = 0x8000, Replacement = 0xa000, Other = 0xc000;
  ASSERT_EQ(llvm::toString(CPU->map(Replacement, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias, 0x1234, 2)), "");
  struct ReplacementCase {
    std::vector<GuestAliasRange> Remove;
    std::vector<GuestAliasMapping> Add;
  };
  const ReplacementCase Invalid[] = {
      {{{Alias, Page}, {Alias, Page}}, {}},
      {{{Data, Page}}, {}},
      {{{Alias, 2 * Page}}, {}},
      {{{Alias, Page}}, {{Alias, Alias, Page, Read}}},
      {{{Alias, Page}}, {{Data, Replacement, Page, Read}}},
      {{{Alias, Page}},
       {{Alias, Replacement, Page, Read}, {Alias, Data, Page, Write}}},
      {{{Alias, Page}},
       {{Alias, Replacement, Page, Read},
        {Other, Data, Page, Read},
        {Other + Page, Data, Page, Read}}},
      {{{Alias, Page}}, {{Alias, UINT64_MAX, Page, Read}}},
      {{{Alias, Page}}, {{Alias, Replacement, Page, Execute << 1}}}};
  for (const auto &Case : Invalid) {
    EXPECT_NE(llvm::toString(CPU->replaceAliases(Case.Remove, Case.Add)), "");
    auto Writable = CPU->canAccess(Alias, Page, Write);
    ASSERT_TRUE(bool(Writable)) << llvm::toString(Writable.takeError());
    EXPECT_TRUE(*Writable);
    EXPECT_EQ(*CPU->readInteger(Alias, 2), 0x1234u);
    EXPECT_FALSE(CPU->fault());
  }
  ASSERT_EQ(llvm::toString(CPU->replaceAliases(
                {{Alias, Page}}, {{Alias, Replacement, Page, Read}})),
            "");
  EXPECT_EQ(*CPU->readInteger(Alias, 2), 0u);
  std::array<uint8_t, 2> Original{};
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data, Original)), "");
  EXPECT_EQ(Original, (std::array<uint8_t, 2>{0x34, 0x12}));
}

TEST_F(DriverBackendBacking,
       AliasRetirementRejectsExecutionAndCallbackReentry) {
  constexpr uint64_t Alias = 0x8000, Device = 0xa000;
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read)), "");
  unsigned Attempts = 0;
  auto Attempt = [&] {
    ++Attempts;
    EXPECT_NE(llvm::toString(CPU->unmapAlias(Alias, Page)), "");
    EXPECT_NE(llvm::toString(CPU->mapAlias(0xc000, Data, Page, Read)), "");
  };
  GuestMMIOCallbacks Callbacks{
      [&](uint64_t, uint64_t, bool) {
        Attempt();
        return llvm::Error::success();
      },
      [](uint64_t, unsigned) -> llvm::Expected<uint64_t> { return 7; },
      [](uint64_t, unsigned, uint64_t) { return llvm::Error::success(); }};
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  auto Value = CPU->readInteger(Device, 1);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(*Value, 7u);
  ASSERT_EQ(llvm::toString(CPU->write(Code, {0x90})), "");
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    Attempt();
    CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_GE(Attempts, 2u);
  EXPECT_EQ(llvm::toString(CPU->validateBacking(Alias, Page)), "");
  EXPECT_FALSE(CPU->fault());
  std::array<uint8_t, 1> Byte{};
  EXPECT_NE(llvm::toString(CPU->read(Data, Byte)), "");
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault);
  EXPECT_NE(llvm::toString(CPU->unmapAlias(Alias, Page)), "");
  EXPECT_EQ(llvm::toString(CPU->snapshotBacking(Alias, Byte)), "");
  EXPECT_EQ(CPU->fault()->Address, Fault->Address);
  EXPECT_EQ(CPU->fault()->Kind, Fault->Kind);
}

TEST_F(DriverBackendBacking, RejectsWholeUnmappedRangeWithoutPrefixEffects) {
  const std::array<uint8_t, 2> Original{0x12, 0x34};
  const std::array<uint8_t, 4> Replacement{9, 8, 7, 6};
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data + Page - 2, Original)), "");
  EXPECT_NE(llvm::toString(CPU->writeBacking(Data + Page - 2, Replacement)),
            "");
  std::array<uint8_t, 4> Destination{0xaa, 0xbb, 0xcc, 0xdd};
  const auto Before = Destination;
  EXPECT_NE(llvm::toString(CPU->readBacking(Data + Page - 2, Destination)), "");
  EXPECT_EQ(Destination, Before);
  std::array<uint8_t, 2> Bytes{};
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data + Page - 2, Bytes)), "");
  EXPECT_EQ(Bytes, Original);
  EXPECT_NE(llvm::toString(CPU->validateBacking(UINT64_MAX - 1, 4)), "");
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendBacking, RejectsMMIOAndMixedSpansWithoutDeviceCallbacks) {
  unsigned Effects = 0;
  GuestMMIOCallbacks Callbacks{
      [&](uint64_t, uint64_t, bool) {
        ++Effects;
        return llvm::Error::success();
      },
      [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
        ++Effects;
        return 0;
      },
      [&](uint64_t, unsigned, uint64_t) {
        ++Effects;
        return llvm::Error::success();
      }};
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Data + Page, Page, Callbacks)), "");
  std::array<uint8_t, 4> Bytes{1, 2, 3, 4};
  for (uint64_t Address : {Data + Page - 2, Data + Page}) {
    EXPECT_NE(llvm::toString(CPU->validateBacking(Address, Bytes.size())), "");
    EXPECT_NE(llvm::toString(CPU->readBacking(Address, Bytes)), "");
    EXPECT_NE(llvm::toString(CPU->writeBacking(Address, Bytes)), "");
    EXPECT_NE(llvm::toString(CPU->snapshotBacking(Address, Bytes)), "");
  }
  EXPECT_EQ(Bytes, (std::array<uint8_t, 4>{1, 2, 3, 4}));
  EXPECT_EQ(Effects, 0u);
  EXPECT_FALSE(CPU->fault());
  EXPECT_FALSE(CPU->hasDeviceError());
}

TEST_F(DriverBackendBacking, RejectsRunningCPUWithoutPoisoningNormalBoundary) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, {0x90})), "");
  unsigned Attempts = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    ++Attempts;
    std::array<uint8_t, 1> Byte{1};
    EXPECT_NE(llvm::toString(CPU->validateBacking(Data, 1)), "");
    EXPECT_NE(llvm::toString(CPU->readBacking(Data, Byte)), "");
    EXPECT_NE(llvm::toString(CPU->writeBacking(Data, Byte)), "");
    EXPECT_NE(llvm::toString(CPU->snapshotBacking(Data, Byte)), "");
    CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_EQ(Attempts, 1u);
  EXPECT_FALSE(CPU->fault());
  EXPECT_EQ(llvm::toString(CPU->validateBacking(Data, 1)), "");
}

TEST_F(DriverBackendBacking, RejectsDeviceCallbackReentryWithoutRAMEffects) {
  unsigned Attempts = 0;
  const std::array<uint8_t, 1> Original{0x35};
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data, Original)), "");
  auto Attempt = [&] {
    ++Attempts;
    std::array<uint8_t, 1> Byte{0xff};
    EXPECT_NE(llvm::toString(CPU->validateBacking(Data, 1)), "");
    EXPECT_NE(llvm::toString(CPU->readBacking(Data, Byte)), "");
    EXPECT_NE(llvm::toString(CPU->writeBacking(Data, Byte)), "");
    EXPECT_NE(llvm::toString(CPU->snapshotBacking(Data, Byte)), "");
    EXPECT_EQ(Byte[0], 0xff);
  };
  GuestMMIOCallbacks Callbacks{
      [&](uint64_t, uint64_t, bool) {
        Attempt();
        return llvm::Error::success();
      },
      [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
        Attempt();
        return 7;
      },
      [](uint64_t, unsigned, uint64_t) { return llvm::Error::success(); }};
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Data + Page, Page, Callbacks)), "");
  auto Value = CPU->readInteger(Data + Page, 1);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(*Value, 7u);
  EXPECT_GT(Attempts, 0u);
  std::array<uint8_t, 1> Byte{};
  ASSERT_EQ(llvm::toString(CPU->readBacking(Data, Byte)), "");
  EXPECT_EQ(Byte, Original);
}

TEST_F(DriverBackendBacking, PreservesFirstFaultAndRejectsLaterEffects) {
  std::array<uint8_t, 1> Byte{0xa5};
  EXPECT_NE(llvm::toString(CPU->read(Data + 2 * Page, Byte)), "");
  const auto Original = CPU->fault();
  ASSERT_TRUE(Original);
  EXPECT_NE(llvm::toString(CPU->validateBacking(Data, 1)), "");
  EXPECT_NE(llvm::toString(CPU->readBacking(Data, Byte)), "");
  EXPECT_NE(llvm::toString(CPU->writeBacking(Data, Byte)), "");
  EXPECT_EQ(Byte[0], 0xa5);
  EXPECT_EQ(CPU->fault()->Kind, Original->Kind);
  EXPECT_EQ(CPU->fault()->PC, Original->PC);
  EXPECT_EQ(CPU->fault()->Address, Original->Address);
  EXPECT_NE(llvm::toString(CPU->run(Code, 1000000)), "");
}

TEST_F(DriverBackendBacking, DeviceFailureAlsoPreventsBackingAccessAndResume) {
  GuestMMIOCallbacks Callbacks{
      [](uint64_t, uint64_t, bool) { return llvm::Error::success(); },
      [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "injected device failure");
      },
      [](uint64_t, unsigned, uint64_t) { return llvm::Error::success(); }};
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Data + Page, Page, Callbacks)), "");
  auto Value = CPU->readInteger(Data + Page, 1);
  ASSERT_FALSE(bool(Value));
  EXPECT_NE(llvm::toString(Value.takeError()).find("injected device failure"),
            std::string::npos);
  ASSERT_TRUE(CPU->hasDeviceError());
  EXPECT_FALSE(CPU->fault());
  std::array<uint8_t, 1> Byte{0x12};
  EXPECT_NE(llvm::toString(CPU->validateBacking(Data, 1)), "");
  EXPECT_NE(llvm::toString(CPU->readBacking(Data, Byte)), "");
  EXPECT_NE(llvm::toString(CPU->writeBacking(Data, Byte)), "");
  EXPECT_EQ(Byte[0], 0x12);
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Byte)), "");
  EXPECT_EQ(Byte[0], 0);
  EXPECT_NE(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_TRUE(CPU->hasDeviceError());
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendBacking, SnapshotPreservesTerminalFaultAndSharedRAM) {
  constexpr uint64_t Alias = 0x8000;
  ASSERT_EQ(llvm::toString(CPU->map(Data + Page, Page, 0)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, Page, Read | Write)), "");
  const std::array<uint8_t, 4> Original{1, 2, 3, 4};
  ASSERT_EQ(llvm::toString(CPU->writeBacking(Data + Page - 2, Original)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias + Page - 1, 0xa5, 1)), "");
  std::array<uint8_t, 4> Bytes{};
  EXPECT_NE(llvm::toString(CPU->read(Data, Bytes)), "");
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault);
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data + Page - 2, Bytes)), "");
  EXPECT_EQ(Bytes, (std::array<uint8_t, 4>{1, 0xa5, 3, 4}));
  std::array<uint8_t, 2> AliasBytes{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Alias + Page - 2, AliasBytes)),
            "");
  EXPECT_EQ(AliasBytes, (std::array<uint8_t, 2>{1, 0xa5}));
  EXPECT_EQ(CPU->fault()->Kind, Fault->Kind);
  EXPECT_EQ(CPU->fault()->PC, Fault->PC);
  EXPECT_EQ(CPU->fault()->Address, Fault->Address);
  EXPECT_NE(llvm::toString(CPU->readBacking(Data, Bytes)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, 1000000)), "");
}

TEST_F(DriverBackendBacking, SnapshotRejectsWholeInvalidRangeWithoutEffects) {
  std::array<uint8_t, 4> Bytes{1, 2, 3, 4};
  const auto Original = Bytes;
  for (uint64_t Address : {Data + Page - 2, UINT64_MAX - 1}) {
    EXPECT_NE(llvm::toString(CPU->snapshotBacking(Address, Bytes)), "");
    EXPECT_EQ(Bytes, Original);
  }
  EXPECT_FALSE(CPU->fault());
  EXPECT_FALSE(CPU->hasDeviceError());
}

class OrdinaryMemoryOnly final : public GuestMemory {
public:
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return llvm::Error::success();
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return llvm::Error::success();
  }
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override {
    return llvm::Error::success();
  }
};
TEST(DriverBackendBackingOptional, OrdinaryImplementationsRejectDeviceAccess) {
  OrdinaryMemoryOnly Memory;
  std::array<uint8_t, 1> Byte{};
  EXPECT_NE(llvm::toString(Memory.validateBacking(0, 1)), "");
  EXPECT_NE(llvm::toString(Memory.readBacking(0, Byte)), "");
  EXPECT_NE(llvm::toString(Memory.writeBacking(0, Byte)), "");
  EXPECT_NE(llvm::toString(Memory.snapshotBacking(0, Byte)), "");
  EXPECT_NE(llvm::toString(Memory.unmapAlias(0, 1)), "");
  EXPECT_NE(llvm::toString(Memory.replaceAliases({}, {})), "");
}
} // namespace
} // namespace neverd::emulation

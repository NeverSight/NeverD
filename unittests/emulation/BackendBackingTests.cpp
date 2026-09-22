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
  EXPECT_NE(llvm::toString(CPU->run(Code, 1000000)), "");
  EXPECT_TRUE(CPU->hasDeviceError());
  EXPECT_FALSE(CPU->fault());
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
}
} // namespace
} // namespace neverd::emulation

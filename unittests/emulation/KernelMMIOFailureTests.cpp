//===- KernelMMIOFailureTests.cpp - Mapping failure ownership -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inject mapping failures at the GuestMemory boundary and verify that virtual
/// identities, physical state and ownership survive unsuccessful operations.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelMMIO.h"

#include <map>

namespace neverd::emulation {
namespace {
llvm::Error memoryFailure(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

class FailingMMIOMemory final : public GuestMemory {
public:
  enum class Failure { None, Budget, Engine };
  struct Region {
    uint64_t Size;
    GuestMMIOCallbacks Callbacks;
  };
  std::map<uint64_t, Region> Regions;
  std::vector<uint64_t> MapAttempts;
  Failure NextMapFailure = Failure::None;
  bool FailNextUnmap = false;

  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return memoryFailure("RAM mapping is outside this test");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return memoryFailure("protection is outside this test");
  }
  llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                      GuestMMIOCallbacks Callbacks) override {
    MapAttempts.push_back(Address);
    const auto Failure = NextMapFailure;
    NextMapFailure = FailingMMIOMemory::Failure::None;
    if (Failure == FailingMMIOMemory::Failure::Budget)
      return llvm::make_error<GuestMemoryLimitError>();
    if (Failure == FailingMMIOMemory::Failure::Engine)
      return memoryFailure("injected engine mapping failure");
    if (!Regions.emplace(Address, Region{Size, std::move(Callbacks)}).second)
      return memoryFailure("duplicate test mapping");
    return llvm::Error::success();
  }
  llvm::Error unmapMMIO(uint64_t Address, uint64_t Size) override {
    if (FailNextUnmap) {
      FailNextUnmap = false;
      return memoryFailure("injected engine unmapping failure");
    }
    auto I = Regions.find(Address);
    if (I == Regions.end() || I->second.Size != Size)
      return memoryFailure("unmap lost exact test mapping");
    Regions.erase(I);
    return llvm::Error::success();
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    auto I = region(Address, Bytes.size());
    if (I == Regions.end())
      return memoryFailure("read lost its mapping");
    const uint64_t Offset = Address - I->first;
    auto &Callbacks = I->second.Callbacks;
    if (auto E = Callbacks.Validate(Offset, Bytes.size(), false))
      return E;
    auto Value = Callbacks.Read(Offset, Bytes.size());
    if (!Value)
      return Value.takeError();
    for (size_t N = 0; N < Bytes.size(); ++N)
      Bytes[N] = uint8_t(*Value >> (8 * N));
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override {
    auto I = region(Address, Bytes.size());
    if (I == Regions.end())
      return memoryFailure("write lost its mapping");
    const uint64_t Offset = Address - I->first;
    auto &Callbacks = I->second.Callbacks;
    if (auto E = Callbacks.Validate(Offset, Bytes.size(), true))
      return E;
    uint64_t Value = 0;
    for (size_t N = 0; N < Bytes.size(); ++N)
      Value |= uint64_t(Bytes[N]) << (8 * N);
    return Callbacks.Write(Offset, Bytes.size(), Value);
  }

private:
  auto region(uint64_t Address, uint64_t Size)
      -> std::map<uint64_t, Region>::iterator {
    auto I = Regions.upper_bound(Address);
    if (I == Regions.begin())
      return Regions.end();
    --I;
    if (Address - I->first >= I->second.Size ||
        Size > I->second.Size - (Address - I->first))
      return Regions.end();
    return I;
  }
};

class KernelMMIOFailure : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x1230;
  static constexpr uint64_t Physical = 0xf0000004;
  FailingMMIOMemory Memory;
  KernelMMIO Model{Memory};

  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void SetUp() override {
    DriverPnpDevice Config;
    Config.ID = "fault_bank";
    Config.Bus = DriverBusKind::RegisterBank;
    Config.InitialDevicePower = DevicePowerState::D0;
    Config.Resources.push_back({"registers",
                                0x4004,
                                Physical,
                                0x100,
                                {{0, 4, DriverRegisterAccess::ReadWrite, 17}}});
    ASSERT_EQ(llvm::toString(Model.configure(PDO, Config)), "");
    ASSERT_EQ(llvm::toString(Model.beginStart(PDO)), "");
    ASSERT_EQ(llvm::toString(Model.completeLowerStart(PDO, 0)), "");
    ASSERT_EQ(llvm::toString(Model.finishPnp(PDO, DevicePnpRequest::Start, 0)),
              "");
  }
  llvm::Expected<uint64_t> map() {
    return Model.map(Physical, 4, mmio::NonCached, false);
  }
};

TEST_F(KernelMMIOFailure, BudgetFailureReturnsNullAndPreservesNextIdentity) {
  Memory.NextMapFailure = FailingMMIOMemory::Failure::Budget;
  EXPECT_EQ(take(map()), 0u);
  EXPECT_TRUE(Memory.Regions.empty());
  ASSERT_EQ(llvm::toString(Model.canRemove(PDO)), "");
  ASSERT_EQ(Memory.MapAttempts.size(), 1u);
  const auto Retry = take(map());
  ASSERT_NE(Retry, 0u);
  ASSERT_EQ(Memory.MapAttempts.size(), 2u);
  EXPECT_EQ(Memory.MapAttempts[0], Memory.MapAttempts[1]);
  EXPECT_EQ(Retry - Memory.MapAttempts[1], Physical % 4096);
  EXPECT_EQ(take(Memory.readInteger(Retry, 4)), 17u);
}

TEST_F(KernelMMIOFailure, EngineFailureRemainsErrorWithoutPublishingMapping) {
  Memory.NextMapFailure = FailingMMIOMemory::Failure::Engine;
  auto Result = map();
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()),
            "injected engine mapping failure");
  EXPECT_TRUE(Memory.Regions.empty());
  ASSERT_EQ(llvm::toString(Model.canRemove(PDO)), "");
  const auto Retry = take(map());
  ASSERT_EQ(Memory.MapAttempts.size(), 2u);
  EXPECT_EQ(Memory.MapAttempts[0], Memory.MapAttempts[1]);
  EXPECT_EQ(take(Memory.readInteger(Retry, 4)), 17u);
}

TEST_F(KernelMMIOFailure, FailedUnmapPreservesAliasAndOwnershipForRetry) {
  const auto First = take(map()), Alias = take(map());
  ASSERT_NE(First, Alias);
  ASSERT_EQ(llvm::toString(Memory.writeInteger(First, 91, 4)), "");
  Memory.FailNextUnmap = true;
  EXPECT_EQ(llvm::toString(Model.unmap(First, 4)),
            "injected engine unmapping failure");
  EXPECT_EQ(Memory.Regions.size(), 2u);
  EXPECT_EQ(take(Memory.readInteger(First, 4)), 91u);
  EXPECT_EQ(take(Memory.readInteger(Alias, 4)), 91u);
  EXPECT_NE(llvm::toString(Model.canRemove(PDO)).find("mapping"),
            std::string::npos);
  ASSERT_EQ(llvm::toString(Model.unmap(First, 4)), "");
  EXPECT_EQ(Memory.Regions.size(), 1u);
  EXPECT_EQ(take(Memory.readInteger(Alias, 4)), 91u);
  ASSERT_EQ(llvm::toString(Model.unmap(Alias, 4)), "");
  EXPECT_TRUE(Memory.Regions.empty());
  ASSERT_EQ(llvm::toString(Model.canRemove(PDO)), "");
}
} // namespace
} // namespace neverd::emulation

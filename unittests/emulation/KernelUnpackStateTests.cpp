//===- KernelUnpackStateTests.cpp - Owned kernel recovery capture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent allocation, lifetime and diagnostic capture contracts.
///
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelAPINames.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using PoolAPI = WindowsDriverState::PoolAPI;
constexpr uint64_t Scratch = 0x60000000;
constexpr uint64_t MemoryLimit = 16 * 1024 * 1024;
constexpr uint32_t Tag = 0x61747354;

class KernelUnpackState : public testing::Test {
protected:
  std::shared_ptr<AddressSpace> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void ok(llvm::Error Error) {
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name.str(), Arguments));
  }
  void reason(const WindowsDriverState &State, llvm::StringRef Text) {
    EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(), Text),
              State.Reasons.end());
    EXPECT_TRUE(State.hasAdditionalDependencies());
    EXPECT_EQ(State.additionalDependencyReasons(),
              take(Model->unpackDependencies()));
  }
  void start(DriverOptions Options = {}) {
    Model.reset();
    Result = DriverResult{};
    Memory = take(AddressSpace::create(
        take(PhysicalMemory::create(MemoryLimit)), MemoryLimit));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 4096, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
  }
  void SetUp() override { start(); }
};

TEST_F(KernelUnpackState,
       NativeBugCheckRegistrationsRemainDependenciesUntilRemoved) {
  DriverOptions O;
  O.Contract = ExecutionContract::CheckedX64;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  start(O);
  ok(Memory->map(Scratch + 4096, 4096, Read | Execute));
  const uint8_t Name[] = {'n', 0};
  ok(Memory->write(Scratch + 256, Name));
  ok(Model->captureUnpackBaseline());
  ASSERT_EQ(call(kernel_api::KeRegisterBugCheckReasonCallback,
                 {Scratch, Scratch + 4096, 2, Scratch + 256}),
            1u);
  reason(take(Model->captureUnpackRuntime(false)),
         "live kernel bug-check callbacks");
  ASSERT_EQ(call(kernel_api::KeDeregisterBugCheckReasonCallback, {Scratch}),
            1u);
  const auto Removed = take(Model->captureUnpackRuntime(false));
  EXPECT_EQ(std::count(Removed.Reasons.begin(), Removed.Reasons.end(),
                       "live kernel bug-check callbacks"),
            0u);
}

TEST_F(KernelUnpackState, RequiresAnOwnershipBaseline) {
  for (const bool IncludeBacking : {false, true}) {
    auto State = Model->captureUnpackRuntime(IncludeBacking);
    ASSERT_FALSE(bool(State));
    EXPECT_EQ(llvm::toString(State.takeError()),
              "kernel recovery has no ownership baseline");
  }
  ok(Model->captureUnpackBaseline());
  const auto State = take(Model->captureUnpackRuntime(true));
  EXPECT_EQ(State.Profile, ProcessRuntimeState::Kind::WindowsDriverX64);
  EXPECT_TRUE(State.Pools.empty());
  EXPECT_FALSE(State.Entry);
  EXPECT_TRUE(State.Reasons.empty());
  EXPECT_FALSE(State.hasAdditionalDependencies());
}

TEST_F(KernelUnpackState, RetainsTheExactLegacyAllocationContract) {
  ok(Model->captureUnpackBaseline());
  const auto Untagged = call(kernel_api::ExAllocatePool, {0x123400000000, 17});
  const auto Paged =
      call(kernel_api::ExAllocatePoolWithTag, {1, 33, 0x123400000000 | Tag});
  const auto NX = call(kernel_api::ExAllocatePoolWithTag, {512, 4097, Tag});
  const auto Metadata = take(Model->captureUnpackRuntime(false));
  ASSERT_EQ(Metadata.Pools.size(), 3u);
  const uint64_t Addresses[] = {Untagged, Paged, NX};
  const uint64_t Sizes[] = {17, 33, 4097};
  const uint64_t Types[] = {0, 1, 512};
  for (size_t I = 0; I < 3; ++I) {
    const auto &Pool = Metadata.Pools[I];
    EXPECT_EQ(Pool.Address, Addresses[I]);
    EXPECT_EQ(Pool.RequestedSize, Sizes[I]);
    EXPECT_EQ(Pool.TypeOrFlags, Types[I]);
    EXPECT_EQ(Pool.API,
              I ? PoolAPI::ExAllocatePoolWithTag : PoolAPI::ExAllocatePool);
    EXPECT_EQ(Pool.Tag, I ? Tag : 0u);
    EXPECT_FALSE(Pool.Bytes.has_value());
  }
  const auto Contents = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(Contents.Pools.size(), 3u);
  for (size_t I = 0; I < 3; ++I) {
    ASSERT_TRUE(Contents.Pools[I].Bytes);
    EXPECT_EQ(*Contents.Pools[I].Bytes, std::vector<uint8_t>(Sizes[I], 0xcd));
  }
  reason(Contents, "live kernel pool allocations");
  EXPECT_EQ(Contents.Reasons,
            (std::vector<std::string>{"live kernel pool allocations"}));
}

TEST_F(KernelUnpackState, RetainsModernFlagsAndInitializationSemantics) {
  ok(Model->captureUnpackBaseline());
  // Preserve optional high flags and the distinction between zeroed and
  // explicitly uninitialized storage. Values are from the public pool ABI.
  const uint64_t Flags[] = {0x100, 0x10000000040, 0x4a};
  for (const auto Flag : Flags)
    ASSERT_NE(call(kernel_api::ExAllocatePool2, {Flag, 29, Tag}), 0u);
  const auto State = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(State.Pools.size(), 3u);
  for (size_t I = 0; I < 3; ++I) {
    const auto &Pool = State.Pools[I];
    EXPECT_EQ(Pool.API, PoolAPI::ExAllocatePool2);
    EXPECT_EQ(Pool.TypeOrFlags, Flags[I]);
    EXPECT_EQ(Pool.Tag, Tag);
    EXPECT_EQ(Pool.RequestedSize, 29u);
    ASSERT_TRUE(Pool.Bytes);
    EXPECT_EQ(*Pool.Bytes, std::vector<uint8_t>(29, I == 2 ? 0xcd : 0));
  }
  EXPECT_EQ(State.Pools[2].Address % 64, 0u);
  reason(State, "live kernel pool allocations");
  EXPECT_EQ(State.Reasons,
            (std::vector<std::string>{"live kernel pool allocations"}));
}

TEST_F(KernelUnpackState, TypedPoolsCannotHideOtherRetainedObjects) {
  ok(Model->captureUnpackBaseline());
  const auto Pool = call(kernel_api::ExAllocatePool2, {0x40, 64, Tag});
  ASSERT_NE(Pool, 0u);
  ASSERT_EQ(call(kernel_api::IoCreateDevice,
                 {Model->driverObject(), 32, 0, 0x22, 0, 0, Scratch}),
            0u);
  const auto Mixed = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(Mixed.Pools.size(), 1u);
  EXPECT_EQ(Mixed.Pools.front().Address, Pool);
  reason(Mixed, "live kernel pool allocations");
  reason(Mixed, "retained kernel object allocations");
  reason(Mixed, "changed kernel loader objects");
  reason(Mixed, "unreconstructed kernel or environment effects");
  call(kernel_api::ExFreePoolWithTag, {Pool, Tag});
  const auto ReleasedPool = take(Model->captureUnpackRuntime(true));
  EXPECT_TRUE(ReleasedPool.Pools.empty());
  EXPECT_EQ(std::find(ReleasedPool.Reasons.begin(), ReleasedPool.Reasons.end(),
                      "live kernel pool allocations"),
            ReleasedPool.Reasons.end());
  reason(ReleasedPool, "retained kernel object allocations");
  reason(ReleasedPool, "changed kernel loader objects");
  reason(ReleasedPool, "unreconstructed kernel or environment effects");
}

TEST_F(KernelUnpackState, OwnsCurrentBytesAcrossMutationAndRelease) {
  ok(Model->captureUnpackBaseline());
  const auto First = call(kernel_api::ExAllocatePool2, {0x40, 19, Tag});
  const auto Second = call(kernel_api::ExAllocatePool2, {0x40, 23, Tag});
  std::vector<uint8_t> Bytes(19, 0x37);
  // This integer can be a pointer or ordinary data. Capture preserves it
  // verbatim; allocation identity alone cannot authorize a pointer rewrite.
  llvm::support::endian::write64le(Bytes.data() + 3, Second + 7);
  ok(Memory->write(First, Bytes));
  const auto Before = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(Before.Pools.size(), 2u);
  ASSERT_TRUE(Before.Pools[0].Bytes);
  EXPECT_EQ(*Before.Pools[0].Bytes, Bytes);
  ok(Memory->writeInteger(First + 3, 0, 8));
  call(kernel_api::ExFreePool, {First});
  const auto Third = call(kernel_api::ExAllocatePool2, {0x40, 19, Tag});
  EXPECT_NE(Third, First);
  const auto After = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(After.Pools.size(), 2u);
  EXPECT_EQ(After.Pools[0].Address, Second);
  EXPECT_EQ(After.Pools[1].Address, Third);
  EXPECT_EQ(Before.Pools[0].Address, First);
  EXPECT_EQ(*Before.Pools[0].Bytes, Bytes);
}

TEST_F(KernelUnpackState, ReleasedPoolsLeaveNoReconstructionRecord) {
  ok(Model->captureUnpackBaseline());
  const auto Address = call(kernel_api::ExAllocatePoolWithTag, {512, 31, Tag});
  call(kernel_api::ExFreePoolWithTag, {Address, Tag});
  const auto State = take(Model->captureUnpackRuntime(true));
  EXPECT_TRUE(State.Pools.empty());
  EXPECT_TRUE(State.Reasons.empty());
  EXPECT_FALSE(State.hasAdditionalDependencies());
}

TEST_F(KernelUnpackState, FailedAllocationAndFreePreserveLifetimeIdentity) {
  ok(Model->captureUnpackBaseline());
  const auto Address = call(kernel_api::ExAllocatePool2, {0x40, 21, Tag});
  EXPECT_EQ(call(kernel_api::ExAllocatePool2, {0x40, UINT64_MAX, Tag}), 0u);
  auto Free =
      Model->call(kernel_api::ExFreePoolWithTag.str(), {Address, Tag ^ 1});
  ASSERT_FALSE(bool(Free));
  llvm::consumeError(Free.takeError());
  const auto State = take(Model->captureUnpackRuntime(true));
  ASSERT_EQ(State.Pools.size(), 1u);
  EXPECT_EQ(State.Pools[0].Address, Address);
  EXPECT_EQ(State.Pools[0].Tag, Tag);
  ASSERT_TRUE(State.Pools[0].Bytes);
  EXPECT_EQ(*State.Pools[0].Bytes, std::vector<uint8_t>(21, 0));
}

TEST_F(KernelUnpackState, RequiredBackingCannotBecomeAnEmptySnapshot) {
  ok(Model->captureUnpackBaseline());
  const auto Address = call(kernel_api::ExAllocatePool2, {0x40, 8192, Tag});
  ASSERT_EQ(Address % 4096, 0u);
  ok(Memory->unmap(Address, 4096));
  const auto Metadata = take(Model->captureUnpackRuntime(false));
  ASSERT_EQ(Metadata.Pools.size(), 1u);
  EXPECT_EQ(Metadata.Pools[0].Address, Address);
  EXPECT_FALSE(Metadata.Pools[0].Bytes);
  auto Contents = Model->captureUnpackRuntime(true);
  ASSERT_FALSE(bool(Contents));
  llvm::consumeError(Contents.takeError());
}

TEST_F(KernelUnpackState, BorrowedAndDeviceObjectsAreNotPoolAllocations) {
  ok(Model->captureUnpackBaseline());
  EXPECT_TRUE(take(Model->captureUnpackRuntime(true)).Pools.empty());
  ASSERT_EQ(call(kernel_api::IoCreateDevice,
                 {Model->driverObject(), 32, 0, 0x22, 0, 0, Scratch}),
            0u);
  const auto State = take(Model->captureUnpackRuntime(true));
  EXPECT_TRUE(State.Pools.empty());
  reason(State, "retained kernel object allocations");
  reason(State, "changed kernel loader objects");
  reason(State, "unreconstructed kernel or environment effects");
}

TEST_F(KernelUnpackState, OpaqueEffectsAndEntryLifetimeRemainDependencies) {
  ok(Model->captureUnpackBaseline());
  Model->recordUnpackEnvironmentRead();
  const auto Before = take(Model->captureUnpackRuntime(false));
  EXPECT_TRUE(Before.Pools.empty());
  reason(Before, "unreconstructed kernel or environment effects");
  ok(Model->finishEntry());
  const auto After = take(Model->captureUnpackRuntime(true));
  EXPECT_TRUE(After.Pools.empty());
  reason(After, "unreconstructed kernel or environment effects");
  reason(After, "DriverEntry has already returned");
  reason(After, "released kernel loader objects");
}
} // namespace
} // namespace neverd::emulation

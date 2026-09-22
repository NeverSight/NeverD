//===- KernelRemoveLocksTests.cpp - Remove-lock ownership ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify exact storage ownership, opaque tags, atomic failures and drain
/// readiness independently of PnP state and guest callback execution.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelRemoveLocks.h"

#include <array>
#include <limits>
#include <utility>

namespace neverd::emulation {
namespace {

constexpr uint64_t FirstLock = 0x1100;
constexpr uint64_t SecondLock = 0x2100;
constexpr uint64_t FirstDevice = 0x1000;
constexpr uint64_t SecondDevice = 0x2000;
constexpr uint32_t RetailSize = remove_lock::RetailSize;
constexpr uint32_t DebugSize = remove_lock::DebugSize;

void success(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

template <class T>
void expectError(llvm::Expected<T> Result, llvm::StringRef Text) {
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find(Text.str()),
            std::string::npos);
}

void expectError(llvm::Error E, llvm::StringRef Text) {
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
}

class DriverKernelRemoveLocks : public ::testing::Test {
protected:
  KernelRemoveLocks Locks;

  void initialize(uint64_t Lock = FirstLock, uint64_t Owner = FirstDevice,
                  uint32_t Size = RetailSize) {
    success(Locks.initialize({Lock, Owner, Size}));
  }
};

TEST_F(DriverKernelRemoveLocks, RegistrationPreservesExactDeviceAndExtent) {
  initialize();
  initialize(SecondLock, SecondDevice, DebugSize);
  initialize(SecondLock + DebugSize, SecondDevice);
  const auto First = take(Locks.registration(FirstLock));
  EXPECT_EQ(First.Address, FirstLock);
  EXPECT_EQ(First.OwnerDevice, FirstDevice);
  EXPECT_EQ(First.Size, RetailSize);
  const auto Second = take(Locks.registration(SecondLock));
  EXPECT_EQ(Second.Address, SecondLock);
  EXPECT_EQ(Second.OwnerDevice, SecondDevice);
  EXPECT_EQ(Second.Size, DebugSize);
  EXPECT_EQ(take(Locks.registration(SecondLock + DebugSize)).OwnerDevice,
            SecondDevice);
}

TEST_F(DriverKernelRemoveLocks, MalformedRegistrationDoesNotPublishIdentity) {
  const std::array<KernelRemoveLocks::Registration, 5> Invalid = {{
      {0, FirstDevice, RetailSize},
      {FirstLock, 0, RetailSize},
      {FirstLock + 1, FirstDevice, RetailSize},
      {FirstLock, FirstDevice, RetailSize - 1},
      {std::numeric_limits<uint64_t>::max() - 7, FirstDevice, RetailSize},
  }};
  for (const auto &Registration : Invalid) {
    auto E = Locks.initialize(Registration);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    expectError(Locks.registration(Registration.Address), "unknown");
  }
  initialize();
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 0)));
}

TEST_F(DriverKernelRemoveLocks, OverlappingExtentsFailButAdjacencyIsValid) {
  initialize(FirstLock, FirstDevice, DebugSize);
  expectError(Locks.initialize({FirstLock + 8, SecondDevice, RetailSize}),
              "opaque");
  expectError(Locks.initialize({FirstLock - 8, SecondDevice, RetailSize}),
              "opaque");
  expectError(Locks.initialize({FirstLock - 8, SecondDevice, DebugSize}),
              "opaque");
  initialize(FirstLock - RetailSize, SecondDevice);
  initialize(FirstLock + DebugSize, SecondDevice);
  EXPECT_EQ(take(Locks.registration(FirstLock)).OwnerDevice, FirstDevice);
  expectError(Locks.registration(FirstLock + 8), "unknown");
}

TEST_F(DriverKernelRemoveLocks, CannotReinitializeLiveOrDrainedIdentity) {
  initialize();
  expectError(Locks.initialize({FirstLock, SecondDevice, DebugSize}),
              "already initialized");
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 0)));
  EXPECT_TRUE(take(Locks.releaseAndWait(FirstLock, RetailSize, 0)));
  expectError(Locks.initialize({FirstLock, FirstDevice, RetailSize}),
              "already initialized");
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
  EXPECT_EQ(take(Locks.registration(FirstLock)).OwnerDevice, FirstDevice);
}

TEST_F(DriverKernelRemoveLocks, RepeatedNullTagsDrainOneAcquisitionAtATime) {
  initialize();
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 0)));
  EXPECT_FALSE(take(Locks.releaseAndWait(FirstLock, RetailSize, 0)));
  EXPECT_FALSE(take(Locks.acquire(FirstLock, RetailSize, 0)));
  success(Locks.release(FirstLock, RetailSize, 0));
  EXPECT_FALSE(take(Locks.drained(FirstLock)));
  success(Locks.release(FirstLock, RetailSize, 0));
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
  EXPECT_FALSE(take(Locks.acquire(FirstLock, RetailSize, 0)));
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
  expectError(Locks.release(FirstLock, RetailSize, 0), "no matching");
}

TEST_F(DriverKernelRemoveLocks, OrdinaryZeroReferencesDoesNotCloseAdmission) {
  initialize();
  EXPECT_FALSE(take(Locks.drained(FirstLock)));
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 55)));
  success(Locks.release(FirstLock, RetailSize, 55));
  EXPECT_FALSE(take(Locks.drained(FirstLock)));
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 55)));
  EXPECT_TRUE(take(Locks.releaseAndWait(FirstLock, RetailSize, 55)));
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
}

TEST_F(DriverKernelRemoveLocks, OpaqueRetiredAddressTagsDoNotCrossRelease) {
  initialize();
  initialize(SecondLock, SecondDevice);
  const uint64_t Tag = std::numeric_limits<uint64_t>::max();
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, Tag)));
  EXPECT_TRUE(take(Locks.acquire(SecondLock, RetailSize, Tag)));
  EXPECT_TRUE(take(Locks.acquire(SecondLock, RetailSize, SecondDevice)));
  EXPECT_TRUE(take(Locks.releaseAndWait(FirstLock, RetailSize, Tag)));
  EXPECT_FALSE(take(Locks.drained(SecondLock)));
  expectError(Locks.release(FirstLock, RetailSize, SecondDevice),
              "no matching");
  EXPECT_FALSE(take(Locks.releaseAndWait(SecondLock, RetailSize, Tag)));
  success(Locks.release(SecondLock, RetailSize, SecondDevice));
  EXPECT_TRUE(take(Locks.drained(SecondLock)));
}

TEST_F(DriverKernelRemoveLocks, SizeMismatchDoesNotAcquireReleaseOrClose) {
  initialize();
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 1)));
  expectError(Locks.acquire(FirstLock, DebugSize, 2), "size");
  expectError(Locks.release(FirstLock, DebugSize, 1), "size");
  expectError(Locks.releaseAndWait(FirstLock, DebugSize, 1), "size");
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 3)));
  expectError(Locks.release(FirstLock, RetailSize, 2), "no matching");
  EXPECT_FALSE(take(Locks.releaseAndWait(FirstLock, RetailSize, 1)));
  success(Locks.release(FirstLock, RetailSize, 3));
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
}

TEST_F(DriverKernelRemoveLocks, UnmatchedDrainFailsBeforeClosingAdmission) {
  initialize();
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 1)));
  expectError(Locks.releaseAndWait(FirstLock, RetailSize, 2), "no matching");
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 2)));
  EXPECT_FALSE(take(Locks.releaseAndWait(FirstLock, RetailSize, 1)));
  expectError(Locks.releaseAndWait(FirstLock, RetailSize, 2),
              "already draining");
  EXPECT_FALSE(take(Locks.drained(FirstLock)));
  success(Locks.release(FirstLock, RetailSize, 2));
  EXPECT_TRUE(take(Locks.drained(FirstLock)));
}

TEST_F(DriverKernelRemoveLocks, PartialRetirementPreservesWholeRegistration) {
  initialize();
  expectError(Locks.forgetRange(FirstLock + 1, RetailSize), "partial");
  expectError(Locks.forgetRange(FirstLock - 1, RetailSize), "partial");
  expectError(Locks.forgetRange(FirstLock + 8, 1), "partial");
  success(Locks.forgetRange(FirstLock, 0));
  success(Locks.forgetRange(FirstLock - 1, 1));
  success(Locks.forgetRange(FirstLock + RetailSize, 1));
  EXPECT_EQ(take(Locks.registration(FirstLock)).Size, RetailSize);
  success(Locks.forgetRange(FirstLock - 8, RetailSize + 16));
  expectError(Locks.registration(FirstLock), "unknown");
}

TEST_F(DriverKernelRemoveLocks, RetirementBatchIsAtomicAcrossHeldLocks) {
  initialize();
  initialize(FirstLock + RetailSize, SecondDevice);
  EXPECT_TRUE(take(Locks.acquire(FirstLock + RetailSize, RetailSize, 0)));
  expectError(Locks.canReleaseRange(FirstLock, RetailSize * 2), "acquisitions");
  expectError(Locks.forgetRange(FirstLock, RetailSize * 2), "acquisitions");
  EXPECT_EQ(take(Locks.registration(FirstLock)).OwnerDevice, FirstDevice);
  EXPECT_EQ(take(Locks.registration(FirstLock + RetailSize)).OwnerDevice,
            SecondDevice);
  success(Locks.release(FirstLock + RetailSize, RetailSize, 0));
  success(Locks.forgetRange(FirstLock, RetailSize * 2));
  expectError(Locks.registration(FirstLock), "unknown");
  expectError(Locks.registration(FirstLock + RetailSize), "unknown");
}

TEST_F(DriverKernelRemoveLocks, FailedAddDeviceMayRetireUnusedInitializedLock) {
  initialize();
  EXPECT_FALSE(take(Locks.drained(FirstLock)));
  success(Locks.canReleaseRange(FirstDevice, 0x200));
  success(Locks.forgetRange(FirstDevice, 0x200));
  success(Locks.validateGuestAccess(FirstLock, RetailSize, true));
  expectError(Locks.registration(FirstLock), "unknown");
}

TEST_F(DriverKernelRemoveLocks,
       DrainedRetirementInvalidatesEveryLockOperation) {
  initialize();
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 1)));
  EXPECT_TRUE(take(Locks.releaseAndWait(FirstLock, RetailSize, 1)));
  success(Locks.forgetRange(FirstLock, RetailSize));
  expectError(Locks.acquire(FirstLock, RetailSize, 1), "unknown");
  expectError(Locks.release(FirstLock, RetailSize, 1), "unknown");
  expectError(Locks.releaseAndWait(FirstLock, RetailSize, 1), "unknown");
  expectError(Locks.drained(FirstLock), "unknown");
  initialize(FirstLock, SecondDevice, DebugSize);
  EXPECT_EQ(take(Locks.registration(FirstLock)).OwnerDevice, SecondDevice);
  EXPECT_TRUE(take(Locks.acquire(FirstLock, DebugSize, 1)));
}

TEST_F(DriverKernelRemoveLocks,
       OpaqueStorageChecksBothDirectionsAndBoundaries) {
  initialize(FirstLock, FirstDevice, DebugSize);
  for (bool IsWrite : {false, true}) {
    expectError(Locks.validateGuestAccess(FirstLock, 1, IsWrite), "opaque");
    expectError(
        Locks.validateGuestAccess(FirstLock + DebugSize - 1, 1, IsWrite),
        "opaque");
    expectError(Locks.validateGuestAccess(FirstLock - 1, 2, IsWrite), "opaque");
    expectError(
        Locks.validateGuestAccess(FirstLock - 8, DebugSize + 16, IsWrite),
        "opaque");
    success(Locks.validateGuestAccess(FirstLock - 1, 1, IsWrite));
    success(Locks.validateGuestAccess(FirstLock + DebugSize, 1, IsWrite));
    success(Locks.validateGuestAccess(FirstLock, 0, IsWrite));
  }
  const uint64_t Maximum = std::numeric_limits<uint64_t>::max();
  expectError(Locks.validateGuestAccess(Maximum, 1, false), "overflow");
  expectError(Locks.canReleaseRange(Maximum, 1), "overflow");
  expectError(Locks.forgetRange(Maximum, 1), "overflow");
  success(Locks.validateGuestAccess(Maximum, 0, false));
  success(Locks.forgetRange(Maximum, 0));
  EXPECT_EQ(take(Locks.registration(FirstLock)).Size, DebugSize);
}

TEST_F(DriverKernelRemoveLocks,
       RegistrationBudgetFailureDoesNotReserveStorage) {
  for (uint64_t I = 0; I < remove_lock::MaxLocks; ++I)
    initialize(FirstLock + I * RetailSize);
  const uint64_t Extra = FirstLock + remove_lock::MaxLocks * RetailSize;
  expectError(Locks.initialize({Extra, SecondDevice, RetailSize}), "capacity");
  expectError(Locks.registration(Extra), "unknown");
  success(Locks.validateGuestAccess(Extra, RetailSize, true));
  success(Locks.forgetRange(FirstLock, RetailSize));
  initialize(Extra, SecondDevice);
  EXPECT_EQ(take(Locks.registration(Extra)).OwnerDevice, SecondDevice);
}

TEST_F(DriverKernelRemoveLocks, SharedAcquisitionBudgetIsTransactional) {
  initialize();
  initialize(SecondLock, SecondDevice);
  for (uint64_t I = 0; I + 1 < remove_lock::MaxReferences; ++I)
    ASSERT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 1)));
  EXPECT_TRUE(take(Locks.acquire(SecondLock, RetailSize, 2)));
  expectError(Locks.acquire(FirstLock, RetailSize, 1), "capacity");
  expectError(Locks.acquire(SecondLock, RetailSize, 3), "capacity");
  expectError(Locks.release(SecondLock, RetailSize, 3), "no matching");
  success(Locks.release(FirstLock, RetailSize, 1));
  EXPECT_TRUE(take(Locks.acquire(SecondLock, RetailSize, 3)));
  EXPECT_FALSE(take(Locks.releaseAndWait(SecondLock, RetailSize, 2)));
  // Restore the global budget to full; closed admission still reports the
  // documented acquisition failure rather than an unrelated model limit.
  EXPECT_TRUE(take(Locks.acquire(FirstLock, RetailSize, 1)));
  EXPECT_FALSE(take(Locks.acquire(SecondLock, RetailSize, 4)));
  success(Locks.release(SecondLock, RetailSize, 3));
  EXPECT_TRUE(take(Locks.drained(SecondLock)));
}

} // namespace
} // namespace neverd::emulation

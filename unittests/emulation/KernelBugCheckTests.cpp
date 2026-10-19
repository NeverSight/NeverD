//===- KernelBugCheckTests.cpp - Callback ownership and lifetime ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint64_t Page = 4096, Base = 0x180000000, Record = Base + Page + 128;
constexpr uint64_t Scratch = 0x60000000, Limit = 16 * 1024 * 1024;
void checked(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T checked(llvm::Expected<T> V) {
  if (!V) {
    ADD_FAILURE() << llvm::toString(V.takeError());
    return {};
  }
  return std::move(*V);
}
void denied(llvm::Error E, llvm::StringRef Fragment = {}) {
  ASSERT_TRUE(bool(E));
  const auto Text = llvm::toString(std::move(E));
  EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
}
template <class T>
void denied(llvm::Expected<T> V, llvm::StringRef Fragment = {}) {
  ASSERT_FALSE(bool(V));
  denied(V.takeError(), Fragment);
}
struct Session {
  std::shared_ptr<AddressSpace> Memory = checked(
      AddressSpace::create(checked(PhysicalMemory::create(Limit)), Limit));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  Session(ExecutionContract Contract = ExecutionContract::CheckedX64) {
    checked(Memory->map(Base, 4 * Page, Read | Write));
    checked(Memory->protect(Base, Page, Read | Execute));
    checked(Memory->map(Scratch, Page, Read | Write));
    DriverImage Image;
    Image.Base = Base;
    Image.Size = 4 * Page;
    Image.Entry = Base;
    // The last mapped page is deliberately not loader-owned image RAM.
    Image.Regions.push_back({Base, Read | Execute, std::vector<uint8_t>(Page)});
    Image.Regions.push_back(
        {Base + Page, Read | Write, std::vector<uint8_t>(2 * Page)});
    DriverOptions Options;
    Options.Contract = Contract;
    checked(Model.initialize(Image, Options));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    return checked(Model.call(Name, Args));
  }
  uint64_t reg(uint64_t R = Record, unsigned Reason = 2,
               uint64_t Callback = Base, uint64_t Component = Scratch) {
    return call("KeRegisterBugCheckReasonCallback",
                {R, Callback, Reason, Component});
  }
  uint64_t remove(uint64_t R = Record) {
    return call("KeDeregisterBugCheckReasonCallback", {R});
  }
  uint64_t integer(uint64_t A, unsigned Width) {
    return checked(Memory->readInteger(A, Width));
  }
  std::vector<uint8_t> bytes(uint64_t A = Record, unsigned Size = 48) {
    std::vector<uint8_t> B(Size);
    checked(Memory->snapshotBacking(A, B));
    return B;
  }
};

TEST(KernelBugCheck, RegistrationDuplicatesRemovalAndReusePreserveIRQL) {
  Session S;
  S.Model.enterExecution(profile::StackBase);
  for (unsigned Level : {0u, 2u, 15u}) {
    if (Level)
      EXPECT_EQ(S.call("KfRaiseIrql", {Level}), 0u);
    for (unsigned Reason = 2; Reason <= 7; ++Reason) {
      EXPECT_EQ(S.remove(), 0u);
      EXPECT_EQ(S.reg(Record, Reason), 1u);
      EXPECT_EQ(S.integer(Record + 44, 1), 1u);
      const auto Before = S.bytes();
      // The duplicate does not consult its new, invalid tuple.
      EXPECT_EQ(S.reg(Record, 99, UINT64_MAX, UINT64_MAX), 0u);
      EXPECT_EQ(S.bytes(), Before);
      EXPECT_EQ(S.remove(), 1u);
      EXPECT_EQ(S.integer(Record + 44, 1), 0u);
      EXPECT_EQ(S.integer(Record + 40, 4), Reason);
      EXPECT_EQ(S.remove(), 0u);
      EXPECT_EQ(S.reg(Record, Reason, Base + 8, Scratch + 8), 1u);
      EXPECT_EQ(S.integer(Record + 16, 8), Base + 8);
      EXPECT_EQ(S.integer(Record + 24, 8), Scratch + 8);
      EXPECT_EQ(S.remove(), 1u);
      EXPECT_EQ(S.Model.currentIRQL(), Level);
      EXPECT_FALSE(S.Model.takeGuestCall());
    }
    if (Level)
      S.call("KeLowerIrql", {0});
  }
  checked(S.Model.validateExecutionReturn(profile::StackBase, 0));
}

TEST(KernelBugCheck, OpaqueBytesAndAllWritesStayGuardedAfterRemoval) {
  Session S;
  EXPECT_EQ(S.reg(), 1u);
  for (bool Removed : {false, true}) {
    if (Removed)
      EXPECT_EQ(S.remove(), 1u);
    const auto Before = S.bytes();
    for (unsigned I = 0; I < 48; ++I) {
      SCOPED_TRACE(I);
      const bool Readable = (I >= 16 && I < 32) || (I >= 40 && I < 45);
      auto E = S.Model.validateGuestAccess(Record + I, 1, false);
      EXPECT_EQ(bool(E), !Readable);
      llvm::consumeError(std::move(E));
      denied(S.Model.validateGuestAccess(Record + I, 1, true), "read-only");
    }
    for (auto [Offset, Width] : {std::pair{15u, 2u}, {24u, 16u}, {40u, 8u}})
      denied(S.Model.validateGuestAccess(Record + Offset, Width, false),
             "opaque");
    S.call("memcpy", {Scratch, Record + 16, 16});
    EXPECT_EQ(S.integer(Scratch, 8), Base);
    const auto Output = S.bytes(Scratch, 48);
    denied(S.Model.call("memcpy", {Scratch, Record, 48}), "opaque");
    EXPECT_EQ(S.bytes(Scratch, 48), Output);
    denied(S.Model.call("RtlZeroMemory", {Record, 48}), "read-only");
    denied(S.Model.call("memcpy", {Record + 44, Scratch, 1}), "read-only");
    const KernelExportRegistry::Export Counter{"KeQueryPerformanceCounter",
                                               "hal.dll"};
    denied(S.Model.call(Counter, {Record + 16}, nullptr), "read-only");
    denied(S.Model.call("RtlInitUnicodeString", {Record + 16, 0}), "read-only");
    EXPECT_EQ(S.bytes(), Before);
  }
}

TEST(KernelBugCheck, PreconditionsRejectWithoutRecordEffects) {
  Session S;
  const auto Before = S.bytes();
  const auto Pool = S.call("ExAllocatePool2", {0x40, 64, 0});
  for (uint64_t R : {uint64_t(0), Record + 1, Scratch, Pool, Base + 3 * Page,
                     Base + 3 * Page - 40, UINT64_MAX - 7}) {
    denied(
        S.Model.call("KeRegisterBugCheckReasonCallback", {R, Base, 2, Scratch}),
        "driver-image");
    denied(S.Model.call("KeDeregisterBugCheckReasonCallback", {R}),
           "driver-image");
  }
  for (uint64_t Callback : {Scratch, Record, Base + 3 * Page, UINT64_MAX})
    denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                        {Record, Callback, 2, Scratch}),
           "driver code");
  for (uint64_t Reason : {0, 1, 8, 0x3ff})
    denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                        {Record, Base, Reason, Scratch}),
           "reason");
  for (uint64_t Name : {uint64_t(0), Scratch + Page, UINT64_MAX})
    denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                        {Record, Base, 2, Name}));
  checked(S.Memory->write(Scratch, std::vector<uint8_t>(256, 'a')));
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record, Base, 2, Scratch}),
         "256-byte");
  checked(S.Memory->protect(Base + Page, Page, Read));
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record, Base, 2, Scratch}),
         "writable RAM");
  EXPECT_EQ(S.bytes(), Before);
}

TEST(KernelBugCheck, UnknownOrAlteredStateIsNotAFalseResult) {
  Session S;
  checked(S.Memory->writeInteger(Record + 44, 1, 1));
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record, Base, 2, Scratch}),
         "unknown record state");
  denied(S.Model.call("KeDeregisterBugCheckReasonCallback", {Record}),
         "unknown record state");
  checked(S.Memory->writeInteger(Record + 44, 0, 1));
  EXPECT_EQ(S.reg(), 1u);
  for (unsigned Offset : {16u, 24u, 40u, 44u}) {
    const auto Old = S.integer(Record + Offset, 1);
    checked(S.Memory->writeInteger(Record + Offset, Old ^ 1, 1));
    denied(S.Model.call("KeDeregisterBugCheckReasonCallback", {Record}),
           "owned state");
    checked(S.Memory->writeInteger(Record + Offset, Old, 1));
  }
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record + 16, Base, 2, Scratch}));
  EXPECT_EQ(S.remove(), 1u);
}

TEST(KernelBugCheck, LegacyCannotActivateOpaqueRecords) {
  Session S(ExecutionContract::Legacy);
  const auto Before = S.bytes();
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record, Base, 2, Scratch}),
         "checked-x64");
  denied(S.Model.call("KeDeregisterBugCheckReasonCallback", {Record}),
         "checked-x64");
  EXPECT_EQ(S.bytes(), Before);
}

TEST(KernelBugCheck, DuplicateAndRemovalDoNotRereadExpiredComponent) {
  Session S;
  EXPECT_EQ(S.reg(), 1u);
  checked(S.Memory->unmap(Scratch, Page));
  EXPECT_EQ(S.reg(), 0u);
  EXPECT_EQ(S.remove(), 1u);
  EXPECT_EQ(S.remove(), 0u);
  denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                      {Record, Base, 2, Scratch}),
         "readable RAM");
}

TEST(KernelBugCheck, EitherPinnedPageOfCrossPageRecordExcludesRegistration) {
  for (uint64_t LockedPage : {Base + Page, Base + 2 * Page}) {
    Session S;
    const uint64_t Cross = Base + 2 * Page - 24;
    // ByteCount is disjoint from the record, but the alias exposes its page.
    const auto MDL = S.call("IoAllocateMdl", {LockedPage + 128, 1, 0, 0, 0});
    S.call("MmProbeAndLockPages", {MDL, 0, 1});
    const auto Alias =
        S.call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 0});
    ASSERT_NE(Alias, 0u);
    const auto Before = S.bytes(Cross);
    denied(S.Model.call("KeRegisterBugCheckReasonCallback",
                        {Cross, Base, 2, Scratch}),
           "physical locks");
    EXPECT_EQ(S.bytes(Cross), Before);
    S.call("MmUnmapLockedPages", {Alias, MDL});
    S.call("MmUnlockPages", {MDL});
    S.call("IoFreeMdl", {MDL});
    EXPECT_EQ(S.reg(Cross), 1u);
    EXPECT_EQ(S.remove(Cross), 1u);
  }
}

TEST(KernelBugCheck, FuturePageLocksCannotExposeLiveOrRetiredOpaqueBytes) {
  Session S;
  const uint64_t Cross = Base + 2 * Page - 24;
  EXPECT_EQ(S.reg(Cross), 1u);
  for (bool Removed : {false, true}) {
    if (Removed)
      EXPECT_EQ(S.remove(Cross), 1u);
    for (uint64_t A : {Base + Page + 128, Base + 2 * Page + 128, Cross + 16}) {
      const auto MDL = S.call("IoAllocateMdl", {A, 1, 0, 0, 0});
      const auto Before = S.bytes(MDL, 56);
      denied(S.Model.call("MmProbeAndLockPages", {MDL, 0, 0}), "page borrow");
      EXPECT_EQ(S.bytes(MDL, 56), Before);
      denied(S.Model.call("MmGetSystemAddressForMdlSafe", {MDL, 0}));
      S.call("IoFreeMdl", {MDL});
    }
  }
  // A disjoint image page keeps the ordinary MDL contract.
  const auto MDL = S.call("IoAllocateMdl", {Base, 1, 0, 0, 0});
  S.call("MmProbeAndLockPages", {MDL, 0, 0});
  S.call("MmUnlockPages", {MDL});
  S.call("IoFreeMdl", {MDL});
}

TEST(KernelBugCheck, UnloadCanRemoveCallbacksButCannotLeaveThemLive) {
  Session S;
  checked(S.Memory->writeInteger(S.Model.driverObject() + 104, Base, 8));
  EXPECT_EQ(S.reg(), 1u);
  checked(S.Model.finishEntry());
  auto Unload = S.Model.beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  EXPECT_EQ(Unload->PC, Base);
  denied(S.Model.finishUnload(), "live bug-check");
  EXPECT_FALSE(S.Result.UnloadCompleted);
  EXPECT_EQ(S.remove(), 1u);
  checked(S.Model.finishUnload());
  EXPECT_TRUE(S.Result.UnloadCompleted);
}

TEST(KernelBugCheck, EnvironmentDependencySurvivesBaselineAndRemoval) {
  for (bool BeforeBaseline : {true, false}) {
    Session S;
    if (!BeforeBaseline)
      checked(S.Model.captureUnpackBaseline());
    EXPECT_EQ(S.reg(), 1u);
    if (BeforeBaseline)
      checked(S.Model.captureUnpackBaseline());
    auto Reasons = checked(S.Model.unpackDependencies());
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "live kernel bug-check callbacks"),
              Reasons.end());
    EXPECT_EQ(S.remove(), 1u);
    Reasons = checked(S.Model.unpackDependencies());
    EXPECT_EQ(std::find(Reasons.begin(), Reasons.end(),
                        "live kernel bug-check callbacks"),
              Reasons.end());
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
    denied(S.Model.call("KeBugCheckEx", {0, 0, 0, 0, 0}),
           "guest requested bug check");
  }
}

TEST(KernelBugCheck, RetiredIdentitiesCountAgainstTheBound) {
  Session S;
  for (unsigned I = 0; I < 64; ++I) {
    const auto R = Base + Page + I * 48;
    EXPECT_EQ(S.reg(R), 1u);
    EXPECT_EQ(S.remove(R), 1u);
  }
  const auto R = Base + Page + 64 * 48;
  denied(
      S.Model.call("KeRegisterBugCheckReasonCallback", {R, Base, 2, Scratch}),
      "identity limit");
  EXPECT_EQ(S.reg(Base + Page), 1u);
}
} // namespace
} // namespace neverd::emulation

//===- KernelExceptionTests.cpp - API-raised guest exception boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Raised guest statuses preserve CPU/model ownership and remain distinct
/// from fatal model-contract failures and backend memory faults.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelAPIIRQL.h"
#include "windows/KernelException.h"
#include "windows/KernelModel.h"

#include <array>

namespace neverd::emulation {
namespace {
class KernelExceptions : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  static void good(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> static T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    good(Memory->map(Scratch, profile::PageSize, Read | Write));
    good(Memory->setReg(X64Register::PC, Entry));
    good(Memory->setReg(X64Register::SP, Scratch + 0x800));
    good(Memory->setReg(X64Register::AX, 0x1122334455667788));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    good(Model->initialize(Image, DriverOptions{}));
  }
  uint32_t raised(llvm::Expected<uint64_t> Value) {
    if (Value) {
      ADD_FAILURE() << "exception API returned an ordinary value";
      return 0;
    }
    auto Error = Value.takeError();
    EXPECT_TRUE(Error.isA<KernelGuestException>());
    uint32_t Code = 0;
    llvm::handleAllErrors(
        std::move(Error),
        [&](const KernelGuestException &E) { Code = E.code(); },
        [&](const llvm::ErrorInfoBase &E) {
          ADD_FAILURE() << "unexpected model error: " << E.message();
        });
    return Code;
  }
  void modelError(llvm::Expected<uint64_t> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    auto Error = Value.takeError();
    EXPECT_FALSE(Error.isA<KernelGuestException>());
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  std::vector<uint8_t> arenaBytes() {
    std::vector<uint8_t> Bytes(profile::KernelArenaSize);
    good(Memory->read(profile::KernelArenaBase, Bytes));
    return Bytes;
  }
  std::array<uint64_t, 4> registers() {
    return {take(Memory->reg(X64Register::PC)),
            take(Memory->reg(X64Register::SP)),
            take(Memory->reg(X64Register::AX)),
            take(Memory->reg(X64Register::CR8))};
  }
};

TEST_F(KernelExceptions, NoArgumentApisRaiseTheirSpecifiedStatuses) {
  EXPECT_EQ(raised(Model->call("ExRaiseAccessViolation", {})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(raised(Model->call("ExRaiseDatatypeMisalignment", {})),
            exceptions::StatusDatatypeMisalignment);
  EXPECT_FALSE(Memory->fault());
}

TEST_F(KernelExceptions, RaiseStatusUsesOnlyTheNtstatusLowDword) {
  EXPECT_EQ(raised(Model->call("ExRaiseStatus", {0x11223344c0000005ULL})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(raised(Model->call("ExRaiseStatus", {0xaabbccdd80000002ULL})),
            exceptions::StatusDatatypeMisalignment);
  EXPECT_EQ(raised(Model->call("ExRaiseStatus", {0xdeadbeef00000103ULL})),
            0x103u);
  EXPECT_EQ(raised(Model->call("ExRaiseStatus", {0x1234567800000000ULL})), 0u);
}

TEST_F(KernelExceptions, RaisesDoNotMutateCpuMemoryOrCreatePendingCallbacks) {
  const auto Before = arenaBytes();
  const auto Registers = registers();
  const auto Driver = Model->driverObject();
  for (const char *Name : {"ExRaiseStatus", "ExRaiseAccessViolation",
                           "ExRaiseDatatypeMisalignment"}) {
    std::vector<uint64_t> Arguments;
    if (llvm::StringRef(Name) == "ExRaiseStatus")
      Arguments.push_back(exceptions::StatusAccessViolation);
    (void)raised(Model->call(Name, Arguments));
    EXPECT_EQ(arenaBytes(), Before);
    EXPECT_EQ(registers(), Registers);
    EXPECT_EQ(Model->driverObject(), Driver);
    EXPECT_EQ(Model->currentIRQL(), 0);
    EXPECT_FALSE(Model->hasPendingHardwareWork());
    EXPECT_FALSE(Model->takeGuestCall());
    EXPECT_FALSE(Model->takeWait());
    EXPECT_FALSE(Memory->fault());
  }
}

TEST_F(KernelExceptions, HandlingTypedExceptionLeavesOrdinaryApisUsable) {
  EXPECT_EQ(raised(Model->call("ExRaiseAccessViolation", {})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(take(Model->call("RtlFillMemory", {Scratch, 16, 0xAB})), 0u);
  std::array<uint8_t, 16> Bytes{};
  good(Memory->read(Scratch, Bytes));
  for (uint8_t Byte : Bytes)
    EXPECT_EQ(Byte, 0xAB);
  EXPECT_EQ(take(Model->call("KeGetCurrentIrql", {})), 0u);
  good(Model->snapshot());
  EXPECT_FALSE(Memory->fault());
}

TEST_F(KernelExceptions, ArityAndUnknownApiErrorsAreNotGuestExceptions) {
  modelError(Model->call("ExRaiseStatus", {}), "argument count");
  modelError(Model->call("ExRaiseStatus", {1, 2}), "argument count");
  modelError(Model->call("ExRaiseAccessViolation", {1}), "argument count");
  modelError(Model->call("ExRaiseDatatypeMisalignment", {1}), "argument count");
  modelError(Model->call("ExRaiseUnknown", {}), "unknown API");
  EXPECT_FALSE(Memory->fault());
  EXPECT_EQ(raised(Model->call("ExRaiseStatus", {0xc0000005})),
            exceptions::StatusAccessViolation);
}

TEST_F(KernelExceptions, UninitializedModelDoesNotRaiseGuestExceptions) {
  DriverResult Empty;
  KernelModel Uninitialized(*Memory, Empty);
  modelError(Uninitialized.call("ExRaiseStatus", {0xc0000005}), "initialized");
  modelError(Uninitialized.call("ExRaiseAccessViolation", {}), "initialized");
  EXPECT_FALSE(Memory->fault());
}

TEST_F(KernelExceptions, IrqlPreflightRejectsBeforeRaiseAndPreservesDpcOwner) {
  (void)take(Model->call("KeInitializeDpc", {Scratch, Entry, 0}));
  EXPECT_EQ(take(Model->call("KeInsertQueueDpc", {Scratch, 11, 22})), 1u);
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  ASSERT_EQ(Model->currentIRQL(), 2);
  const auto Before = arenaBytes();
  for (const char *Name : {"ExRaiseStatus", "ExRaiseAccessViolation",
                           "ExRaiseDatatypeMisalignment"}) {
    std::vector<uint64_t> Arguments;
    if (llvm::StringRef(Name) == "ExRaiseStatus")
      Arguments.push_back(exceptions::StatusAccessViolation);
    modelError(Model->call(Name, Arguments), "IRQL");
    EXPECT_EQ(Model->currentIRQL(), 2);
    EXPECT_EQ(arenaBytes(), Before);
    EXPECT_FALSE(Memory->fault());
  }
  EXPECT_EQ(take(Model->call("KeGetCurrentIrql", {})), 2u);
  good(Model->finishScheduled(Next->ID));
  Model->enterForeground();
  EXPECT_EQ(raised(Model->call("ExRaiseAccessViolation", {})),
            exceptions::StatusAccessViolation);
}

TEST_F(KernelExceptions, CatalogUsesTheSelectedDocumentedIrqlCeilings) {
  // The profile follows the individual Learn pages. The current WDK SAL for
  // the two fixed-status wrappers permits APC_LEVEL; do not mistake this
  // deliberately narrower modeled policy for a universal Windows restriction.
  EXPECT_EQ(take(maximumKernelIRQL("ExRaiseStatus")), 1);
  EXPECT_EQ(take(maximumKernelIRQL("ExRaiseAccessViolation")), 0);
  EXPECT_EQ(take(maximumKernelIRQL("ExRaiseDatatypeMisalignment")), 0);
}
} // namespace
} // namespace neverd::emulation

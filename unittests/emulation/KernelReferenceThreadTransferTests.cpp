//===- KernelReferenceThreadTransferTests.cpp - Reference transfers
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reference transfers.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/WindowsX64ExecutionPolicy.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
std::filesystem::path transferFixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
DriverOptions transferOptions(ExecutionContract Contract, bool Preemptive) {
  DriverOptions O;
  O.Contract = Contract;
  O.Unload = true;
  O.KernelImage = transferFixture("driver_reference_thread_kernel");
  for (const char *Name :
       {"ReferenceThreadTransfer", "ReferenceThreadNonTransfer",
        "ReferenceThreadMemoryTransfer", "RtlInitUnicodeString",
        "MmGetSystemRoutineAddress", "PsCreateSystemThread",
        "PsTerminateSystemThread", "PsGetCurrentProcessId", "ZwClose",
        "ZwQueryInformationThread", "DbgPrint", "ZwOpenKey", "ZwQueryValueKey"})
    O.KernelExports[Name] = true;
  if (Preemptive)
    O.Scheduling = DriverScheduling{23, 1};
  return O;
}

TEST(ReferenceThreadTransfer, ClassifierAcceptsOnlyOrdinaryRegisterJumps) {
  WindowsX64ExecutionPolicy P;
  auto E = P.initialize();
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xff, 0xe1}, {0x41, 0xff, 0xe0}, {0x49, 0xff, 0xe7}}) {
    auto R = P.isReferenceThreadTransfer(Bytes, 0x1000);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(*R);
  }
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xff, 0x21},
                                         {0xff, 0xd1},
                                         {0xc3},
                                         {0x90},
                                         {0x48, 0x89, 0xc8},
                                         {0xeb, 0},
                                         {0xf2, 0xff, 0xe1},
                                         {0x66, 0xff, 0xe1}}) {
    auto R = P.isReferenceThreadTransfer(Bytes, 0x1000);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_FALSE(*R);
  }
  auto Truncated = P.isReferenceThreadTransfer({0xff}, 0x1000);
  ASSERT_FALSE(bool(Truncated));
  llvm::consumeError(Truncated.takeError());
}

TEST(ReferenceThreadTransfer,
     CodeAuthenticationChecksBytesExtentAndExecuteRole) {
  auto R = KernelReferenceImage::load(
      transferFixture("driver_reference_thread_kernel"), 4 * 1024 * 1024);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  const auto Entry = R->exports().at("ReferenceThreadTransfer") + 1;
  EXPECT_TRUE(R->matchesCode(Entry, {0xff, 0xe1}));
  EXPECT_FALSE(R->matchesCode(Entry, {0xff, 0xe2}));
  EXPECT_FALSE(R->matchesCode(Entry, {}));
  EXPECT_FALSE(R->matchesCode(R->base(), {0x4d, 0x5a}));
  EXPECT_FALSE(R->matchesCode(UINT64_MAX, {0xff, 0xe1}));
}

TEST(ReferenceThreadTransfer,
     ActualJumpPreservesContextAndRegisteredStartAddress) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto R =
          emulateDriver(transferFixture("driver_reference_thread_transfer"),
                        transferOptions(Contract, Preemptive));
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      EXPECT_EQ(R->Messages,
                (std::vector<std::string>{"reference-entry-pass"}));
      unsigned Exits = 0;
      for (const auto &C : R->Calls)
        if (C.Name == "PsTerminateSystemThread") {
          ASSERT_EQ(C.Arguments.size(), 1u);
          EXPECT_EQ(C.Arguments[0], 0u);
          ++Exits;
        }
      EXPECT_EQ(Exits, 1u);
    }
}

TEST(ReferenceThreadTransfer,
     NonTransfersMemoryOperandsAndForegroundStayBlocked) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (const char *Name : {"driver_reference_thread_nontransfer",
                               "driver_reference_thread_memory",
                               "driver_reference_thread_foreground"}) {
        auto R = emulateDriver(transferFixture(Name),
                               transferOptions(Contract, Preemptive));
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::UnsupportedAPI) << R->Diagnostic;
        EXPECT_NE(R->Diagnostic.find("inside reference kernel"),
                  std::string::npos);
        EXPECT_FALSE(R->UnloadCompleted);
        EXPECT_FALSE(
            std::any_of(R->Calls.begin(), R->Calls.end(), [](const auto &C) {
              return C.Name == "PsTerminateSystemThread" ||
                     C.Name == "PsGetCurrentProcessId";
            }));
      }
}

TEST(ReferenceThreadTransfer,
     ExportTrapsAndOtherReferenceAddressesRemainScoped) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (const char *Name :
         {"driver_reference_thread_export", "driver_reference_thread_chain"}) {
      auto R = emulateDriver(transferFixture(Name),
                             transferOptions(Contract, false));
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::UnsupportedAPI) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      const auto Create =
          std::find_if(R->Calls.begin(), R->Calls.end(), [](const auto &C) {
            return C.Name == "PsCreateSystemThread";
          });
      ASSERT_NE(Create, R->Calls.end());
      const bool Chain = llvm::StringRef(Name).ends_with("chain");
      EXPECT_EQ(R->PC, Create->Arguments[5] + (Chain ? 3 : 0));
      EXPECT_NE(R->Diagnostic.find(Chain ? "inside reference kernel"
                                         : "ReferenceThreadTransfer"),
                std::string::npos);
    }
}

TEST(ReferenceThreadTransfer,
     ARealEntryLoopConsumesTheSharedInstructionBudget) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto O = transferOptions(Contract, false);
    O.InstructionLimit = 1000;
    auto R = emulateDriver(transferFixture("driver_reference_thread_loop"), O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::InstructionLimit) << R->Diagnostic;
    EXPECT_EQ(R->Instructions, 1000u);
    EXPECT_EQ(R->NTStatus, 0u);
    EXPECT_FALSE(R->UnloadCompleted);
  }
}
} // namespace
} // namespace neverd::emulation

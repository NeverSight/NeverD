//===- DriverSessionStateTests.cpp - Driver resource lifetime -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Driver resource lifetime.
///

#include "gtest/gtest.h"
#include "os/windows/driver/DriverContextCapture.h"
#include "os/windows/driver/DriverSessionState.h"
#include "os/windows/kernel/KernelContextCapture.h"
#include "os/windows/process/WindowsProcessContext.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <filesystem>

using namespace neverd::emulation;
namespace {
std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
class DriverState : public testing::Test {
protected:
  void SetUp() override {
    auto Build = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                            GuestArchitecture::X64);
    ASSERT_TRUE(bool(Build));
    if (Build->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Build->Reason;
  }
  std::unique_ptr<DriverSessionState>
  create(ExecutionContract Contract = ExecutionContract::Legacy,
         const char *Name = "success") {
    DriverOptions O;
    O.Contract = Contract;
    auto S = DriverSessionState::create(fixture(Name), O);
    if (!S) {
      ADD_FAILURE() << llvm::toString(S.takeError());
      return nullptr;
    }
    return std::move(*S);
  }
};
TEST_F(DriverState, InitializesBothContractsWithoutAdmittingGuestInstructions) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto S = create(Contract);
    ASSERT_TRUE(S);
    ASSERT_TRUE(S->CPU && S->Image && S->Kernel && S->Exceptions && S->Guard &&
                S->Budget);
    EXPECT_EQ(S->Options.Contract, Contract);
    EXPECT_EQ(S->Result.Configuration.Contract, Contract);
    EXPECT_EQ(S->Result.ImageBase, S->Image->Base);
    EXPECT_EQ(S->Result.PC, S->Image->Entry);
    EXPECT_FALSE(S->Result.NTStatus);
    EXPECT_EQ(S->Result.Instructions, 0u);
    EXPECT_EQ(S->Budget->instructions(), 0u);
    EXPECT_EQ(S->Budget->events(), 0u);
    EXPECT_EQ(S->Budget->timePolicy(), ExecutionTimePolicy::AbsoluteDeadline);
    EXPECT_TRUE(S->Result.Calls.empty());
    EXPECT_TRUE(S->Result.Writes.empty());
    EXPECT_TRUE(S->Waiting.empty());
    EXPECT_TRUE(S->ClockCalls.empty());
    ASSERT_TRUE(S->Inputs.Image);
    EXPECT_EQ(S->Inputs.Image->sourcePath(), fixture("success"));
    EXPECT_FALSE(S->Inputs.Image->imageDigest().empty());
    EXPECT_NE(S->Kernel->driverObject(), 0u);
  }
}
TEST_F(DriverState, OwnsInputsAndMappedBytesAfterCallerAndFileLifetimeEnd) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-driver-state", Directory));
  std::filesystem::path Root(std::u8string(Directory.begin(), Directory.end()));
  const auto Path = Root / "source.sys";
  std::filesystem::copy_file(fixture("success"), Path);
  std::unique_ptr<DriverSessionState> State;
  {
    DriverOptions O;
    O.ServiceName = "OwnedState";
    O.CPUID.push_back({0, 0, {7, 1, 2, 3}});
    O.Scheduling = DriverScheduling{23, 1};
    auto S = DriverSessionState::create(Path, O);
    ASSERT_TRUE(bool(S)) << llvm::toString(S.takeError());
    State = std::move(*S);
    O.ServiceName = "ChangedCaller";
    O.CPUID.clear();
    O.Scheduling.reset();
  }
  ASSERT_TRUE(std::filesystem::remove(Path));
  EXPECT_EQ(State->Options.ServiceName, "OwnedState");
  EXPECT_EQ(State->Result.Configuration.ServiceName, "OwnedState");
  EXPECT_TRUE(State->Options.Scheduling);
  ASSERT_TRUE(State->CPUID);
  EXPECT_EQ(State->CPUID->query(0, 0), std::optional<DriverCPUIDRegisters>(
                                           DriverCPUIDRegisters{7, 1, 2, 3}));
  for (const auto &R : State->Image->Regions) {
    std::vector<uint8_t> Bytes(R.Bytes.size());
    ASSERT_EQ(llvm::toString(State->CPU->read(R.Address, Bytes)), "");
    // IAT and CFG patches are intentionally live state; compare the entry's
    // original code bytes below rather than all prepared image metadata.
    if (State->Image->Entry >= R.Address &&
        State->Image->Entry - R.Address < R.Bytes.size()) {
      const auto Offset = State->Image->Entry - R.Address;
      EXPECT_EQ(Bytes[Offset], R.Bytes[Offset]);
    }
  }
  State.reset();
  EXPECT_TRUE(std::filesystem::remove(Root));
}
TEST_F(DriverState,
       ContextProviderStillUsesTheOwnedCpuAndModelAfterCreateReturns) {
  auto S = create(ExecutionContract::CheckedX64);
  ASSERT_TRUE(S);
  auto Buffer = S->Kernel->call("ExAllocatePoolWithTag", {0, 4096, 0x53544154});
  ASSERT_TRUE(bool(Buffer)) << llvm::toString(Buffer.takeError());
  ASSERT_NE(*Buffer, 0u);
  const auto SP = profile::StackBase + profile::StackSize - 256 - 8;
  ASSERT_EQ(llvm::toString(S->CPU->setReg(X64Register::SP, SP)), "");
  ASSERT_EQ(llvm::toString(S->CPU->setReg(X64Register::AX, 73)), "");
  ASSERT_EQ(llvm::toString(S->CPU->writeInteger(SP, S->Image->Entry, 8)), "");
  llvm::cantFail(S->CPU->setReg(X64Register::CX, *Buffer));
  llvm::cantFail(S->CPU->setReg(X64Register::FLAGS, 2));
  auto Read = [Owner = S.get()] { return collectDriverContext(*Owner->CPU); };
  auto Set = [Owner = S.get()](uint64_t Flags) {
    return Owner->CPU->setReg(X64Register::FLAGS, Flags);
  };
  const KernelContextCapture Capture{Read, Set};
  auto Captured =
      S->Kernel->call("RtlCaptureContext", {*Buffer}, nullptr, &Capture);
  ASSERT_TRUE(bool(Captured)) << llvm::toString(Captured.takeError());
  std::vector<uint8_t> Bytes(1232);
  ASSERT_EQ(llvm::toString(S->CPU->read(*Buffer, Bytes)), "");
  auto Context = windows_process::readUnwindContext(Bytes);
  ASSERT_TRUE(bool(Context)) << llvm::toString(Context.takeError());
  EXPECT_EQ(Context->PC, S->Image->Entry);
  EXPECT_EQ(Context->GPR[unsigned(X64Register::SP)], SP + 8);
  EXPECT_EQ(Context->GPR[unsigned(X64Register::AX)], 73u);
  EXPECT_EQ(S->Budget->instructions(), 0u);
}
TEST_F(DriverState, IndependentOwnersRetainSeparateRamModelsAndBudgets) {
  auto A = create();
  auto B = create();
  ASSERT_TRUE(A && B);
  EXPECT_NE(A->CPU->addressSpace(), B->CPU->addressSpace());
  EXPECT_NE(A->Kernel.get(), B->Kernel.get());
  auto Before = B->CPU->readInteger(profile::StackBase, 8);
  ASSERT_TRUE(bool(Before));
  ASSERT_EQ(llvm::toString(A->CPU->writeInteger(profile::StackBase, 77, 8)),
            "");
  EXPECT_EQ(*A->CPU->readInteger(profile::StackBase, 8), 77u);
  EXPECT_EQ(*B->CPU->readInteger(profile::StackBase, 8), *Before);
  EXPECT_TRUE(A->Budget->consumeInstructions(3));
  EXPECT_TRUE(A->Budget->consumeEvents(2));
  EXPECT_EQ(B->Budget->instructions(), 0u);
  EXPECT_EQ(B->Budget->events(), 0u);
}
TEST_F(DriverState, FinalPermissionsAndImportAddressesUseTheCanonicalRegistry) {
  auto S = create(ExecutionContract::Legacy, "driver_io");
  ASSERT_TRUE(S);
  ASSERT_FALSE(S->Image->Imports.empty());
  for (const auto &I : S->Image->Imports) {
    auto Bound = S->Exports.bindImport(I);
    ASSERT_TRUE(bool(Bound));
    auto Slot = S->CPU->readInteger(I.Slot, profile::PointerSize);
    ASSERT_TRUE(bool(Slot));
    EXPECT_EQ(*Slot, *Bound);
  }
  for (const auto &R : S->Image->Regions) {
    auto Allowed = S->CPU->canAccess(R.Address, R.Bytes.size(), R.Permissions);
    ASSERT_TRUE(bool(Allowed));
    EXPECT_TRUE(*Allowed);
    if (!(R.Permissions & Write)) {
      auto Writable = S->CPU->canAccess(R.Address, 1, Write);
      ASSERT_TRUE(bool(Writable));
      EXPECT_FALSE(*Writable);
    }
  }
}
TEST_F(DriverState, InvalidContractsAndLimitsFailBeforeOpeningAnInput) {
  for (unsigned I = 0; I < 3; ++I) {
    DriverOptions O;
    if (I == 0)
      O.Contract = ExecutionContract::Software;
    if (I == 1)
      O.InstructionLimit = 0;
    if (I == 2)
      O.EventLimit = profile::MaxEventLimit + 1;
    auto S = DriverSessionState::create("unavailable-input.sys", O);
    EXPECT_FALSE(bool(S));
    if (!S) {
      const auto Error = llvm::toString(S.takeError());
      EXPECT_TRUE(Error.find("contract") != std::string::npos ||
                  Error.find("limits") != std::string::npos)
          << Error;
    }
  }
}
} // namespace

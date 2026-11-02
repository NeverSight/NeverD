//===- UnicornStateReuseTests.cpp - ARM64 state across native steps -------===//
//
// NeverD Decompiler
//
/// \file
/// Checks complete ARM64 state across consecutive native Unicorn steps.
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_UNICORN_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UnicornStateTransferCases.def"
#undef NEVERD_UNICORN_STATE_VALUE

class UnicornStateReuse : public testing::TestWithParam<bool> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<AArch64Machine> Machine;
  AArch64MachineState State;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(MemoryLimit));
    const unsigned User = GetParam() ? UserAccessible : SupervisorPermission;
    llvm::cantFail(
        Memory->map(Code, memory::PageSize, Read | Write | Execute | User));
    llvm::cantFail(Memory->map(Stack, memory::PageSize, Read | Write | User));
    std::vector<uint8_t> Bytes(memory::PageSize);
    for (size_t I = 0; I < Bytes.size(); I += aarch64::InstructionBytes)
      llvm::support::endian::write32le(Bytes.data() + I, NoOperation);
    llvm::cantFail(Memory->write(Code, Bytes));
    Machine = llvm::cantFail(createUnicornAArch64Machine(*Memory, GetParam()));
    State.UserMode = GetParam();
    for (unsigned I = 0; I < State.Registers.size(); ++I)
      State.Registers[I] = InitialScalar + I;
    State.reg(AArch64Register::PC) = Code;
    State.reg(AArch64Register::SP) = Stack + memory::PageSize - StackAlignment;
    State.reg(AArch64Register::NZCV) = ExpectedNZCV;
    State.reg(AArch64Register::FPCR) = ExpectedFPControl;
    State.reg(AArch64Register::FPSR) = ExpectedFPControl;
    for (unsigned I = 0; I < State.Vectors.size(); ++I)
      State.Vectors[I] = {InitialVectorLow + I, InitialVectorHigh - I};
  }
  llvm::Error step(MachineRunControl Control = {}) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    if (auto E = buildAArch64PageTables(*Memory, GetParam()))
      return E;
    return Machine->step(State, Control);
  }
  void expectNoop() {
    auto Expected = State;
    Expected.reg(AArch64Register::PC) += aarch64::InstructionBytes;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.UserMode, Expected.UserMode);
    EXPECT_EQ(State.Registers, Expected.Registers);
    EXPECT_EQ(State.Vectors, Expected.Vectors);
  }
};

TEST_P(UnicornStateReuse, ConsecutiveStepsCaptureTheCompleteState) {
  for (unsigned I = 0; I < SequentialSteps; ++I)
    ASSERT_NO_FATAL_FAILURE(expectNoop());
}

TEST_P(UnicornStateReuse, CancelledWarmStepRetainsInputAndAllowsRetry) {
  ASSERT_NO_FATAL_FAILURE(expectNoop());
  const auto Before = State;
  std::atomic<bool> Stop{true};
  EXPECT_EQ(llvm::toString(step({{}, &Stop})), diagnostic::UnicornRun);
  EXPECT_EQ(State.Registers, Before.Registers);
  EXPECT_EQ(State.Vectors, Before.Vectors);
  ASSERT_NO_FATAL_FAILURE(expectNoop());
}

TEST_P(UnicornStateReuse, EveryChangedInputRegisterReachesTheEngine) {
  ASSERT_NO_FATAL_FAILURE(expectNoop());
  for (unsigned I = 0; I < State.Registers.size(); ++I) {
    SCOPED_TRACE(I);
    auto Register = AArch64Register(I);
    switch (Register) {
    case AArch64Register::PC:
      State.reg(Register) = Code + memory::PageSize / 2;
      break;
    case AArch64Register::SP:
      State.reg(Register) -= StackAlignment;
      break;
    case AArch64Register::NZCV:
      State.reg(Register) ^= ChangedNZCV;
      break;
    case AArch64Register::FPCR:
      State.reg(Register) ^= ChangedFPControl;
      break;
    case AArch64Register::FPSR:
      State.reg(Register) ^= ChangedFPStatus;
      break;
    default:
      State.reg(Register) ^= IntegerIncrement;
      break;
    }
    ASSERT_NO_FATAL_FAILURE(expectNoop());
  }
  for (unsigned I = 0; I < State.Vectors.size(); ++I) {
    SCOPED_TRACE(I);
    State.Vectors[I][0] ^= InitialVectorLow;
    State.Vectors[I][1] ^= InitialVectorHigh;
    ASSERT_NO_FATAL_FAILURE(expectNoop());
  }
}

TEST_P(UnicornStateReuse, MappingChangesAndCodeWritesKeepTheInputState) {
  ASSERT_NO_FATAL_FAILURE(expectNoop());
  const unsigned User = GetParam() ? UserAccessible : SupervisorPermission;
  ASSERT_EQ(llvm::toString(Memory->map(ExtraMapping, memory::PageSize,
                                       Read | Write | User)),
            "");
  ASSERT_NO_FATAL_FAILURE(expectNoop());
  ASSERT_EQ(llvm::toString(
                Memory->addressSpace()->unmap(ExtraMapping, memory::PageSize)),
            "");
  ASSERT_NO_FATAL_FAILURE(expectNoop());
  uint8_t Bytes[aarch64::InstructionBytes];
  llvm::support::endian::write32le(Bytes, IncrementInteger);
  ASSERT_EQ(
      llvm::toString(Memory->write(State.reg(AArch64Register::PC), Bytes)), "");
  auto Expected = State;
  Expected.reg(AArch64Register::X0) += IntegerIncrement;
  Expected.reg(AArch64Register::PC) += aarch64::InstructionBytes;
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(State.Registers, Expected.Registers);
  EXPECT_EQ(State.Vectors, Expected.Vectors);
}

INSTANTIATE_TEST_SUITE_P(Privileges, UnicornStateReuse, testing::Bool());
} // namespace
} // namespace neverd::emulation

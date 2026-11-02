//===- AArch64FPStateTests.cpp - Complete ARM64 machine transport ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/aarch64/AArch64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_VALUE

using Parameter = std::tuple<ExecutionBackendKind, bool>;
class AArch64FPTransport : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<AArch64Machine> Machine;
  AArch64MachineState State;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    const auto Backend = std::get<0>(GetParam());
    const bool User = std::get<1>(GetParam());
    auto Created = Backend == ExecutionBackendKind::KVM
                       ? createKvmAArch64Machine(*Memory)
                   : Backend == ExecutionBackendKind::WHP
                       ? createWhpAArch64Machine(*Memory)
                   : Backend == ExecutionBackendKind::HVF
                       ? createHvfAArch64Machine(*Memory)
                       : createUnicornAArch64Machine(*Memory, User);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(Backend, GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    Machine = std::move(*Created);
    llvm::cantFail(Memory->map(Code, memory::PageSize,
                               Read | Write | Execute | UserAccessible));
    llvm::cantFail(
        Memory->map(Stack, memory::PageSize, Read | Write | UserAccessible));
    State.UserMode = User;
    for (unsigned Index = 0; Index < State.Registers.size(); ++Index)
      State.Registers[Index] = InitialScalar + Index;
    for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
      State.Vectors[Index] = {InitialVectorLow + Index,
                              InitialVectorHigh - Index};
    State.reg(AArch64Register::SP) = Stack + memory::PageSize - StackAlignment;
    State.reg(AArch64Register::NZCV) = InitialNZCV;
    State.reg(AArch64Register::FPCR) = 0;
    State.reg(AArch64Register::FPSR) = InitialFPSR;
  }
  void step(uint32_t Word) {
    State.reg(AArch64Register::PC) = Code;
    uint8_t Bytes[aarch64::InstructionBytes];
    llvm::support::endian::write32le(Bytes, Word);
    llvm::cantFail(Memory->write(Code, Bytes));
    llvm::cantFail(Memory->beginRun());
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    llvm::cantFail(buildAArch64PageTables(*Memory, State.UserMode));
    ASSERT_EQ(llvm::toString(
                  Machine->step(State, {std::chrono::steady_clock::now() +
                                        std::chrono::microseconds(Timeout)})),
              "");
  }
  void expectState(const AArch64MachineState &Expected) {
    EXPECT_EQ(State.UserMode, Expected.UserMode);
    EXPECT_EQ(State.Registers, Expected.Registers);
    EXPECT_EQ(State.Vectors, Expected.Vectors);
  }
};
TEST_P(AArch64FPTransport, PreservesAllScalarsVectorsAndFPControls) {
  for (unsigned Rounding = 0; Rounding < RoundingModes; ++Rounding) {
    SCOPED_TRACE(Rounding);
    State.reg(AArch64Register::FPCR) = Rounding << RoundingShift;
    auto Expected = State;
    Expected.reg(AArch64Register::PC) = Code + aarch64::InstructionBytes;
    step(Nop);
    expectState(Expected);
  }
}
TEST_P(AArch64FPTransport,
       OriginalInstructionsConsumeAllVectorLanesAndHostChanges) {
  for (unsigned Index = 0; Index < State.Vectors.size(); ++Index) {
    SCOPED_TRACE(Index);
    State.Vectors[Index] = {InitialVectorLow ^ Index,
                            InitialVectorHigh + Index};
    auto Expected = State;
    Expected.reg(AArch64Register::X0) = Expected.Vectors[Index][1];
    Expected.reg(AArch64Register::PC) = Code + aarch64::InstructionBytes;
    step((VectorHighToInteger & ~SourceRegisterMask) |
         (Index << SourceRegisterShift));
    expectState(Expected);
  }
  auto Expected = State;
  Expected.Vectors[LastVector] = {
      State.Vectors[FirstOperand][0] ^ State.Vectors[SecondOperand][0],
      State.Vectors[FirstOperand][1] ^ State.Vectors[SecondOperand][1]};
  Expected.reg(AArch64Register::PC) = Code + aarch64::InstructionBytes;
  step(VectorXor);
  expectState(Expected);
}
TEST_P(AArch64FPTransport, OriginalFloatingAddClearsUpperDestinationLane) {
  State.Vectors[FirstOperand][0] = DoubleOne;
  State.Vectors[SecondOperand][0] = DoubleTwo;
  auto Expected = State;
  Expected.Vectors[LastVector] = {DoubleThree, 0};
  Expected.reg(AArch64Register::PC) = Code + aarch64::InstructionBytes;
  step(AddDouble);
  expectState(Expected);
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64FPTransport,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation

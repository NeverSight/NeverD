//===- X64DivisionTests.cpp - Integer division and recovery ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_X64_TRAP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "X64ExceptionCases.def"
#undef NEVERD_X64_TRAP_VALUE
#define NEVERD_X64_DIVISION_VALUE(Name, Value) constexpr unsigned Name = Value;
#define NEVERD_X64_DIVISION_CASE(Name, Width, AX, DX, Divisor, Q, R, ...)      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64DivisionCases.def"
#undef NEVERD_X64_DIVISION_CASE
#undef NEVERD_X64_DIVISION_VALUE
struct DivisionCase {
  const char *Name;
  unsigned Width;
  uint64_t AX, DX, Divisor, ResultAX, ResultDX;
  llvm::ArrayRef<uint8_t> Bytes;
};
const DivisionCase Cases[] = {
#define NEVERD_X64_DIVISION_CASE(Name, Width, AX, DX, Divisor, Q, R, ...)      \
  {#Name, Width, AX, DX, Divisor, Q, R, Name},
#include "X64DivisionCases.def"
#undef NEVERD_X64_DIVISION_CASE
};
void PrintTo(const DivisionCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter =
    std::tuple<ExecutionBackendKind, ExecutionContract, bool, DivisionCase>;
class X64Division : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  const DivisionCase &testCase() const { return std::get<3>(GetParam()); }
  bool memorySource() const { return std::get<2>(GetParam()); }
  void SetUp() override {
    auto B = createExecutionBackend(std::get<0>(GetParam()),
                                    std::get<1>(GetParam()), Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> Bytes(testCase().Bytes.begin(),
                               testCase().Bytes.end());
    if (memorySource())
      Bytes.back() = (Bytes.back() & OpcodeExtensionMask) | MemoryRM;
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::AX, testCase().AX));
    llvm::cantFail(CPU->setReg(X64Register::DX, testCase().DX));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
    divisor(testCase().Divisor);
  }
  void divisor(uint64_t Value) {
    llvm::cantFail(CPU->setReg(X64Register::CX, memorySource() ? Data : Value));
    llvm::cantFail(CPU->writeInteger(Data, Value, testCase().Width));
  }
  ExecutionExit run(BackendHooks H = {}) {
    if (!H.Instruction)
      H.Instruction = [&](uint64_t PC, uint32_t) {
        if (PC != Code)
          CPU->stop();
      };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectOriginal() {
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), testCase().AX);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), testCase().DX);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
  }
};

TEST_P(X64Division, ComputesQuotientRemainderAndPartialRegisterWrites) {
  unsigned Reads = 0, Writes = 0;
  BackendHooks H;
  H.Read = [&](uint64_t Address, uint32_t Size) {
    EXPECT_EQ(Address, Data);
    EXPECT_EQ(Size, testCase().Width);
    expectOriginal();
    ++Reads;
  };
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  const auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Reads, unsigned(memorySource()));
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), testCase().ResultAX);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), testCase().ResultDX);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
            Code + testCase().Bytes.size());
}

TEST_P(X64Division, UnhandledDivideRemainsATerminalGuestTrap) {
  divisor(0);
  unsigned Interrupts = 0;
  BackendHooks H;
  H.Interrupt = [&](uint32_t Vector) {
    EXPECT_EQ(Vector, DivideVector);
    expectOriginal();
    ++Interrupts;
    CPU->stop();
  };
  const auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestTrap) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Interrupt);
  EXPECT_EQ(Exit.Fault->Interrupt, DivideVector);
  EXPECT_EQ(Exit.Fault->PC, Code);
  EXPECT_EQ(Interrupts, 1u);
  expectOriginal();
  EXPECT_TRUE(CPU->fault());
  EXPECT_FALSE(CPU->takeRecoverableFault());
  EXPECT_NE(llvm::toString(CPU->setReg(X64Register::AX, 0)), "");
}

TEST_P(X64Division,
       RecoverableDivideRequiresConsumptionAndExplicitContinuation) {
  auto Clean = llvm::cantFail(CPU->saveContext());
  divisor(0);
  unsigned Interrupts = 0, Recoveries = 0;
  BackendHooks H;
  H.Interrupt = [&](uint32_t) { ++Interrupts; };
  H.RecoverableFault = [&](const BackendFault &F) {
    EXPECT_EQ(F.Kind, BackendFaultKind::Interrupt);
    EXPECT_EQ(F.Interrupt, DivideVector);
    EXPECT_EQ(F.PC, Code);
    expectOriginal();
    ++Recoveries;
    return true;
  };
  const auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  EXPECT_EQ(Recoveries, 1u);
  EXPECT_EQ(Interrupts, 0u);
  EXPECT_FALSE(CPU->fault());
  auto PendingRun = CPU->runUntilExit(Code, Timeout);
  ASSERT_FALSE(bool(PendingRun));
  llvm::consumeError(PendingRun.takeError());
  EXPECT_NE(llvm::toString(CPU->setReg(X64Register::AX, 0)), "");
  auto PendingContext = CPU->saveContext();
  ASSERT_FALSE(bool(PendingContext));
  llvm::consumeError(PendingContext.takeError());
  EXPECT_NE(llvm::toString(CPU->restoreContext(*Clean)), "");
  auto Fault = CPU->takeRecoverableFault();
  ASSERT_TRUE(Fault);
  EXPECT_EQ(Fault->Interrupt, DivideVector);
  EXPECT_FALSE(CPU->takeRecoverableFault());
  expectOriginal();
  divisor(testCase().Divisor);
  const auto Resumed = run();
  ASSERT_EQ(Resumed.Kind, ExecutionExitKind::Stopped) << Resumed.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), testCase().ResultAX);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), testCase().ResultDX);
}

TEST_P(X64Division, ObserverCanStopBeforeDivisionAndExceptionDelivery) {
  divisor(0);
  unsigned Recoveries = 0;
  BackendHooks H;
  if (memorySource())
    H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  else
    H.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
  H.RecoverableFault = [&](const BackendFault &) {
    ++Recoveries;
    return true;
  };
  const auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Recoveries, 0u);
  EXPECT_FALSE(CPU->fault());
  expectOriginal();
}

INSTANTIATE_TEST_SUITE_P(
    Transports, X64Division,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::CheckedX64,
                                     ExecutionContract::CheckedUserX64),
                     testing::Bool(), testing::ValuesIn(Cases)));
INSTANTIATE_TEST_SUITE_P(
    SoftwareDriver, X64Division,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn),
                     testing::Values(ExecutionContract::Legacy),
                     testing::Bool(), testing::ValuesIn(Cases)));
} // namespace
} // namespace neverd::emulation

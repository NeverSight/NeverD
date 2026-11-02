//===- X64ReturnPrefixTests.cpp - Repeat-prefixed near returns ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_RETURN_PREFIX_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_RETURN_PREFIX_BYTES(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ReturnPrefixCases.def"
#undef NEVERD_RETURN_PREFIX_BYTES
#undef NEVERD_RETURN_PREFIX_VALUE
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_RETURN_PREFIX_BACKEND(Name, Kind, Contract)                     \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64ReturnPrefixCases.def"
#undef NEVERD_RETURN_PREFIX_BACKEND
};
void PrintTo(const Backend &B, std::ostream *OS) { *OS << B.Name; }
struct Rejected {
  const char *Name;
  std::vector<uint8_t> Bytes;
};
const Rejected RejectedForms[] = {
#define NEVERD_RETURN_PREFIX_REJECTED(Name, ...) {#Name, {__VA_ARGS__}},
#include "X64ReturnPrefixCases.def"
#undef NEVERD_RETURN_PREFIX_REJECTED
};
constexpr uint64_t Counts[] = {
#define NEVERD_RETURN_PREFIX_COUNT(Value) Value,
#include "X64ReturnPrefixCases.def"
#undef NEVERD_RETURN_PREFIX_COUNT
};
using State = std::map<CPURegister, RegisterValue>;

class X64ReturnPrefix : public testing::TestWithParam<Backend> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  const uint64_t Stack = Data + Offset;
  void SetUp() override {
    auto B =
        createExecutionBackend(GetParam().Kind, GetParam().Contract, Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    for (auto Base : {Code, Data}) {
      llvm::cantFail(
          CPU->map(Base, Page, Read | Write | Execute | UserAccessible));
      llvm::cantFail(CPU->write(
          Base, std::vector<uint8_t>(Page, Base == Code ? Nop : Fill)));
    }
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::SP, Stack));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->writeInteger(Stack, Target, WordBytes));
  }
  State snapshot() {
    State Result;
    for (unsigned R = unsigned(CPURegister::X64AX);
         R <= unsigned(CPURegister::X64FP7); ++R)
      if (registerMatches(CPURegister(R), GuestArchitecture::X64))
        Result[CPURegister(R)] =
            llvm::cantFail(CPU->readRegister(CPURegister(R)));
    return Result;
  }
  std::vector<uint8_t> memory() {
    std::vector<uint8_t> Bytes(Page);
    llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
    return Bytes;
  }
  /// Run from Code until the instruction at the return target is reached.
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};

TEST_P(X64ReturnPrefix, RepeatPrefixedReturnIsTheOrdinaryNearReturn) {
  llvm::cantFail(CPU->write(Code, Admitted));
  for (uint64_t Count : Counts) {
    llvm::cantFail(CPU->setReg(X64Register::CX, Count));
    llvm::cantFail(CPU->setReg(X64Register::SP, Stack));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    auto Before = snapshot();
    const auto Original = memory();
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t Address, unsigned Size) {
      ++Reads;
      EXPECT_EQ(Address, Stack);
      EXPECT_EQ(Size, WordBytes);
      EXPECT_EQ(snapshot(), Before);
    };
    H.Write = [](uint64_t, unsigned, uint64_t) { ADD_FAILURE(); };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    // Only the stack pointer and the program counter change.
    Before[CPURegister::X64SP][0] = Stack + WordBytes;
    Before[CPURegister::X64PC][0] = Target;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
  }
}

TEST_P(X64ReturnPrefix, ReadObserverStopsBeforeTheTransfer) {
  llvm::cantFail(CPU->write(Code, Admitted));
  const auto Before = snapshot();
  const auto Original = memory();
  BackendHooks H;
  H.Read = [&](uint64_t, unsigned) { CPU->stop(); };
  const auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), Original);
}

TEST_P(X64ReturnPrefix, OtherPrefixedReturnsPublishNoObservationsOrEffects) {
  for (const auto &Form : RejectedForms) {
    SCOPED_TRACE(Form.Name);
    llvm::cantFail(CPU->write(Code, std::vector<uint8_t>(Page, Nop)));
    llvm::cantFail(CPU->write(Code, Form.Bytes));
    llvm::cantFail(CPU->setReg(X64Register::SP, Stack));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    const auto Before = snapshot();
    const auto Original = memory();
    unsigned Observations = 0;
    BackendHooks H;
    H.Read = [&](uint64_t, unsigned) { ++Observations; };
    H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
    // A rejected instruction is terminal for this CPU; use a fresh one.
    SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64ReturnPrefix,
                         testing::ValuesIn(Backends),
                         [](const testing::TestParamInfo<Backend> &P) {
                           return std::string(P.param.Name);
                         });
} // namespace
} // namespace neverd::emulation

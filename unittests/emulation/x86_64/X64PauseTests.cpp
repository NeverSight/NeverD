//===- X64PauseTests.cpp - Native spin-wait hints and bounded loops ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include <map>

namespace neverd::emulation {
namespace {
#define NEVERD_PAUSE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PAUSE_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PauseCases.def"
#undef NEVERD_PAUSE_BYTES
#undef NEVERD_PAUSE_VALUE

struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_PAUSE_BACKEND(Name, Backend, Contract)                          \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "X64PauseCases.def"
#undef NEVERD_PAUSE_BACKEND
};

class X64Pause : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> RAM;
  unsigned Reads = 0, Writes = 0;

  void SetUp() override {
    auto Created =
        createExecutionBackend(GetParam().Backend, GetParam().Contract, Limit);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, Page, Read | Write | UserAccessible));
    RAM.assign(Page, Fill);
    llvm::cantFail(CPU->write(Data, RAM));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::SP, Data + Page));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Seed - N}));
  }

  std::map<CPURegister, RegisterValue> snapshot() {
    std::map<CPURegister, RegisterValue> Result;
    for (unsigned R = unsigned(CPURegister::X64AX);
         R <= unsigned(CPURegister::X64FP7); ++R)
      if (registerMatches(CPURegister(R), GuestArchitecture::X64))
        Result[CPURegister(R)] =
            llvm::cantFail(CPU->readRegister(CPURegister(R)));
    return Result;
  }

  BackendHooks observers() {
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) { ++Reads; };
    H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
    return H;
  }

  void expectMemoryPreserved() {
    std::vector<uint8_t> After(Page);
    llvm::cantFail(CPU->snapshotBacking(Data, After));
    EXPECT_EQ(After, RAM);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(Writes, 0u);
  }
};

TEST_P(X64Pause, PreservesCompleteStateAcrossObserverStopAndResume) {
  llvm::cantFail(CPU->write(Code, Pause));
  llvm::cantFail(CPU->write(Code + sizeof(Pause), Stop));
  auto Expected = snapshot();
  auto Saved = llvm::cantFail(CPU->saveContext());
  unsigned Instructions = 0;
  auto H = observers();
  H.Instruction = [&](uint64_t PC, uint32_t Size) {
    ++Instructions;
    EXPECT_EQ(PC, Code);
    EXPECT_EQ(Size, sizeof(Pause));
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(H));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Instructions, 1u);
  EXPECT_EQ(snapshot(), Expected);
  H.Instruction = [&](uint64_t PC, uint32_t Size) {
    ++Instructions;
    EXPECT_EQ(Size, PC == Code ? sizeof(Pause) : sizeof(Stop));
    if (PC == Code + sizeof(Pause))
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(H));
  for (bool Restore : {false, true}) {
    if (Restore)
      llvm::cantFail(CPU->restoreContext(*Saved));
    Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[CPURegister::X64PC][0] = Code + sizeof(Pause);
    EXPECT_EQ(snapshot(), Expected);
    expectMemoryPreserved();
  }
  EXPECT_EQ(Instructions, 5u);
}

TEST_P(X64Pause, SpinLoopDeadlinePreservesStateAndAllowsResumption) {
  llvm::cantFail(CPU->write(Code, Loop));
  auto Expected = snapshot();
  llvm::cantFail(CPU->installHooks(observers()));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, LoopTimeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Deadline) << Exit.Diagnostic;
  EXPECT_TRUE(Exit.DeadlineReached);
  EXPECT_FALSE(Exit.Fault);
  const uint64_t PC = llvm::cantFail(CPU->reg(X64Register::PC));
  EXPECT_TRUE(PC == Code || PC == Code + sizeof(Pause));
  Expected[CPURegister::X64PC][0] = PC;
  EXPECT_EQ(snapshot(), Expected);
  expectMemoryPreserved();

  llvm::cantFail(CPU->write(Code, Pause));
  llvm::cantFail(CPU->write(Code + sizeof(Pause), Stop));
  auto H = observers();
  H.Instruction = [&](uint64_t Address, uint32_t) {
    if (Address == Code + sizeof(Pause))
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(H));
  Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_FALSE(Exit.DeadlineReached);
  Expected[CPURegister::X64PC][0] = Code + sizeof(Pause);
  EXPECT_EQ(snapshot(), Expected);
  expectMemoryPreserved();
}

TEST_P(X64Pause, LockPrefixIsRejectedBeforeEffects) {
  llvm::cantFail(CPU->write(Code, Locked));
  auto Expected = snapshot();
  llvm::cantFail(CPU->installHooks(observers()));
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
      << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), Expected);
  expectMemoryPreserved();
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Pause,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

//===- X64PushFlagsTests.cpp - Native flag images without monitor state --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include <climits>
#include <map>

namespace neverd::emulation {
namespace {
#define NEVERD_PUSH_FLAGS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PUSH_FLAGS_INSTRUCTION(Name, Width, ...)                        \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_PUSH_FLAGS_REJECTED(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PushFlagsCases.def"
#undef NEVERD_PUSH_FLAGS_REJECTED
#undef NEVERD_PUSH_FLAGS_INSTRUCTION
#undef NEVERD_PUSH_FLAGS_VALUE
struct Instruction {
  const char *Name;
  unsigned Width;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_PUSH_FLAGS_INSTRUCTION(Name, Width, ...) {#Name, Width, Name},
#include "X64PushFlagsCases.def"
#undef NEVERD_PUSH_FLAGS_INSTRUCTION
};
constexpr uint64_t FlagBits[] = {
#define NEVERD_PUSH_FLAGS_BIT(Value) Value,
#include "X64PushFlagsCases.def"
#undef NEVERD_PUSH_FLAGS_BIT
};
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_PUSH_FLAGS_BACKEND(Name, Backend, Contract)                     \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "X64PushFlagsCases.def"
#undef NEVERD_PUSH_FLAGS_BACKEND
};

class X64PushFlags : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Original = std::vector<uint8_t>(2 * Page, Fill);
  uint64_t SP = Data + Page, Length = 0;
  unsigned Width = 0;

  void SetUp() override { initialize(); }
  void initialize() {
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
    // Keep distinct physical owners behind adjacent virtual stack pages.
    for (uint64_t A : {Data, Alias, Data + Page})
      llvm::cantFail(CPU->map(A, Page, Read | Write | UserAccessible));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    llvm::cantFail(CPU->setReg(X64Register::FSBase, Alias));
    llvm::cantFail(CPU->setReg(X64Register::GSBase, Alias));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Seed - N}));
  }
  void prepare(const Instruction &I, uint64_t Stack = Data + Page,
               uint64_t Rflags = Flags) {
    SP = Stack;
    Width = I.Width;
    Length = I.Bytes.size();
    std::vector<uint8_t> Bytes(I.Bytes.begin(), I.Bytes.end());
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->write(Data, Original));
    llvm::cantFail(CPU->setReg(X64Register::SP, SP));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Rflags));
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
  std::vector<uint8_t> memory() {
    std::vector<uint8_t> Bytes(Original.size());
    llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
    return Bytes;
  }
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC == Code + Length)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectPush(std::map<CPURegister, RegisterValue> Expected,
                  uint64_t Address = 0) {
    Expected[CPURegister::X64PC][0] = Code + Length;
    Expected[CPURegister::X64SP][0] = SP - Width;
    EXPECT_EQ(snapshot(), Expected);
    auto Bytes = Original;
    if (!Address)
      Address = SP - Width;
    for (unsigned N = 0; N < Width; ++N)
      Bytes.at(Address - Data + N) =
          uint8_t(Expected[CPURegister::X64FLAGS][0] >> (N * CHAR_BIT));
    EXPECT_EQ(memory(), Bytes);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(SP - Width, Width)) & Trap, 0u);
  }
};

TEST_P(X64PushFlags, ArchitecturalFlagsAndImplicitStackWidths) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (unsigned Mask = 0; Mask < (1u << std::size(FlagBits)); ++Mask) {
      uint64_t Rflags = Reserved;
      for (unsigned N = 0; N < std::size(FlagBits); ++N)
        if (Mask & (1u << N))
          Rflags |= FlagBits[N];
      SCOPED_TRACE(Rflags);
      prepare(I, Data + Page, Rflags);
      const auto Expected = snapshot();
      unsigned Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
      H.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
        ++Writes;
        EXPECT_EQ(Address, SP - Width);
        EXPECT_EQ(Size, Width);
        EXPECT_EQ(Value, Rflags);
      };
      const auto Exit = run(std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(Writes, 1u);
      expectPush(Expected);
    }
  }
}

TEST_P(X64PushFlags, ObserversStopBeforeStackAndRegisterEffects) {
  for (const auto &I : Instructions) {
    prepare(I, Data + Page + I.Width / 2);
    const auto Expected = snapshot();
    auto Saved = llvm::cantFail(CPU->saveContext());
    BackendHooks H;
    H.Write = [&](uint64_t, uint32_t, uint64_t Value) {
      EXPECT_EQ(Value, Flags);
      EXPECT_EQ(snapshot(), Expected);
      CPU->stop();
    };
    ASSERT_EQ(run(std::move(H)).Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    for (bool Restore : {false, true}) {
      if (Restore) {
        llvm::cantFail(CPU->restoreContext(*Saved));
        llvm::cantFail(CPU->write(Data, Original));
      }
      ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
      expectPush(Expected);
    }
  }
}

TEST_P(X64PushFlags, CrossPageFaultCanResumeWithoutPartialPush) {
  for (const auto &I : Instructions) {
    prepare(I, Data + Page + I.Width / 2);
    const auto Expected = snapshot();
    llvm::cantFail(CPU->protect(Data + Page, Page, UserAccessible));
    BackendHooks H;
    H.RecoverableFault = [&](const BackendFault &Fault) {
      EXPECT_EQ(Fault.Address, Data + Page);
      EXPECT_EQ(Fault.Size, Width / 2);
      EXPECT_EQ(Fault.Access, BackendAccessKind::Write);
      return true;
    };
    ASSERT_EQ(run(std::move(H)).Kind, ExecutionExitKind::RecoverableFault);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    ASSERT_TRUE(CPU->takeRecoverableFault());
    llvm::cantFail(
        CPU->protect(Data + Page, Page, Read | Write | UserAccessible));
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    expectPush(Expected);
  }
}

TEST_P(X64PushFlags, ReadOnlyStackFaultPreservesContext) {
  for (const auto &I : Instructions) {
    prepare(I);
    const auto Expected = snapshot();
    llvm::cantFail(CPU->protect(Data, Page, Read | UserAccessible));
    BackendHooks H;
    H.RecoverableFault = [](const BackendFault &) { return true; };
    ASSERT_EQ(run(std::move(H)).Kind, ExecutionExitKind::RecoverableFault);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    const auto Fault = CPU->takeRecoverableFault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(Fault->Address, SP - Width);
    EXPECT_EQ(Fault->Size, Width);
    llvm::cantFail(CPU->protect(Data, Page, Read | Write | UserAccessible));
  }
}

TEST_P(X64PushFlags, UserModeCannotWriteSupervisorStack) {
  prepare(Instructions[0], Data + Page + Instructions[0].Width / 2);
  const auto Expected = snapshot();
  llvm::cantFail(CPU->protect(Data + Page, Page, Read | Write));
  const auto Exit = run();
  if (GetParam().Contract == ExecutionContract::CheckedUserX64) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(Exit.Fault->Address, Data + Page);
    EXPECT_EQ(memory(), Original);
    EXPECT_EQ(snapshot(), Expected);
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectPush(Expected);
  }
}

TEST_P(X64PushFlags, AliasedCrossPageStackPublishesCompleteImage) {
  llvm::cantFail(
      CPU->mapAlias(Alias + Page, Data, Page, Read | Write | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Alias + 2 * Page, Data + Page, Page,
                               Read | Write | UserAccessible));
  for (const auto &I : Instructions) {
    prepare(I, Alias + 2 * Page + I.Width / 2);
    const auto Expected = snapshot();
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectPush(Expected, Data + Page - I.Width / 2);
  }
}

TEST_P(X64PushFlags, NoncanonicalAndWrappedStackHaveNoEffects) {
  for (uint64_t Stack : {Noncanonical, Wrapped}) {
    prepare(Instructions[0], Stack);
    const auto Expected = snapshot();
    BackendHooks H;
    H.RecoverableFault = [](const BackendFault &) { return true; };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    ASSERT_TRUE(CPU->takeRecoverableFault());
  }
}

TEST_P(X64PushFlags, LockedFlagsStayUnsupported) {
  for (llvm::ArrayRef<uint8_t> Bytes : {
#define NEVERD_PUSH_FLAGS_REJECTED(Name, ...) llvm::ArrayRef<uint8_t>(Name),
#include "X64PushFlagsCases.def"
#undef NEVERD_PUSH_FLAGS_REJECTED
       }) {
    // Invalid decoding leaves a terminal fault; each encoding owns a CPU.
    initialize();
    ASSERT_FALSE(HasFatalFailure());
    prepare({nullptr, 0, Bytes});
    const auto Expected = snapshot();
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
    H.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
    const auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PushFlags,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

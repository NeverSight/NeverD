//===- X64PopFlagsTests.cpp - Privileged stack flag restoration -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <climits>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_POP_FLAGS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_POP_FLAGS_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_POP_FLAGS_BYTES(Name, ...)                                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PopFlagsCases.def"
#undef NEVERD_POP_FLAGS_BYTES
#undef NEVERD_POP_FLAGS_TEXT
#undef NEVERD_POP_FLAGS_VALUE
constexpr uint64_t FlagBits[] = {
#define NEVERD_POP_FLAGS_BIT(Value) Value,
#include "X64PopFlagsCases.def"
#undef NEVERD_POP_FLAGS_BIT
};
struct Instruction {
  const char *Name;
  unsigned Width;
  std::vector<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_POP_FLAGS_INSTRUCTION(Name, Width, ...)                         \
  {#Name, Width, {__VA_ARGS__}},
#include "X64PopFlagsCases.def"
#undef NEVERD_POP_FLAGS_INSTRUCTION
};
uint64_t admittedFlags(unsigned Mask) {
  uint64_t Result = 0;
  for (unsigned N = 0; N < std::size(FlagBits); ++N)
    if (Mask & (1u << N))
      Result |= FlagBits[N];
  return Result;
}

TEST(X64PopFlagsOracle, OriginalUserInstructionsDetermineFlagsAndStackWidth) {
#if defined(__x86_64__) || defined(_M_X64)
  // No NeverD machine, decoder or mask supplies the actual result.
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
#ifdef _WIN32
    std::vector<uint8_t> Bytes(std::begin(Win64Argument),
                               std::end(Win64Argument));
#else
    std::vector<uint8_t> Bytes(std::begin(SysVArgument),
                               std::end(SysVArgument));
#endif
    Bytes.insert(Bytes.end(), std::begin(OracleBefore), std::end(OracleBefore));
    Bytes.insert(Bytes.end(), I.Bytes.begin(), I.Bytes.end());
    Bytes.insert(Bytes.end(), std::begin(OracleAfter), std::end(OracleAfter));
    std::error_code EC;
    auto Block = llvm::sys::Memory::allocateMappedMemory(
        Page, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
        EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto Release = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
    std::memcpy(Block.base(), Bytes.data(), Bytes.size());
    EC = llvm::sys::Memory::protectMappedMemory(
        Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
    auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
    for (uint64_t Before : {Reserved, Flags})
      for (unsigned Mask = 0; Mask < (1u << std::size(FlagBits)); ++Mask) {
        // Include ignored bits and attempted IOPL changes. TF/AC stay clear
        // in the real host; guest admission of those has separate negatives.
        const uint64_t Input = admittedFlags(Mask) | IgnoredImage | IOPL;
        std::array<uint64_t, 7> Result{Input, Before};
        Execute(Result.data());
        ASSERT_EQ(Result[6] & UserPrivilege, UserPrivilege);
        ASSERT_EQ(Result[4] & IOPL, 0u);
        EXPECT_EQ(Result[2],
                  (Result[5] & ~UserAdmittedMask) | (Input & UserAdmittedMask));
        EXPECT_EQ(Result[3], I.Width);
      }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_POP_FLAGS_BACKEND(Name, Backend, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "X64PopFlagsCases.def"
#undef NEVERD_POP_FLAGS_BACKEND
};
class X64PopFlags : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Original = std::vector<uint8_t>(2 * Page, Fill);
  uint64_t SP = Data + Page, Length = 0, Input = 0;
  unsigned Width = 0;
  bool userMode() const {
    return GetParam().Contract == ExecutionContract::CheckedUserX64;
  }
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
  void prepare(const Instruction &I, uint64_t Value = 0,
               uint64_t Stack = Data + Page, uint64_t Rflags = Flags) {
    SP = Stack;
    Input = Value;
    Width = I.Width;
    Length = I.Bytes.size();
    std::vector<uint8_t> Bytes(I.Bytes);
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    Original.assign(2 * Page, Fill);
    const uint64_t Offset = SP - Data;
    if (Width && Offset <= Original.size() - Width)
      for (unsigned N = 0; N < Width; ++N)
        Original[Offset + N] = uint8_t(Input >> (N * CHAR_BIT));
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
  uint64_t expectedFlags(uint64_t Before) const {
    uint64_t Mask = userMode() ? UserWordMask : SupervisorWordMask;
    if (Width == sizeof(uint64_t))
      Mask |= WideMask;
    return (Before & ~Mask) | (Input & Mask);
  }
  void expectPop(std::map<CPURegister, RegisterValue> Expected) {
    Expected[CPURegister::X64PC][0] = Code + Length;
    Expected[CPURegister::X64SP][0] = SP + Width;
    Expected[CPURegister::X64FLAGS][0] =
        expectedFlags(Expected[CPURegister::X64FLAGS][0]);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
  }
};

TEST_P(X64PopFlags, AdmittedFlagsAndImplicitStackWidths) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (uint64_t Before : {Reserved, Flags})
      for (unsigned Mask = 0; Mask < (1u << std::size(FlagBits)); ++Mask) {
        prepare(I, admittedFlags(Mask), Data + Page, Before);
        const auto Expected = snapshot();
        unsigned Reads = 0;
        BackendHooks H;
        H.Read = [&](uint64_t Address, uint32_t Size) {
          ++Reads;
          EXPECT_EQ(Address, SP);
          EXPECT_EQ(Size, Width);
          EXPECT_EQ(snapshot(), Expected);
        };
        H.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
        const auto Exit = run(std::move(H));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        expectPop(Expected);
      }
  }
}

TEST_P(X64PopFlags, EveryStackBitRespectsPrivilegeAndControlAdmission) {
  for (const auto &I : Instructions)
    for (unsigned Bit = 0; Bit < sizeof(uint64_t) * CHAR_BIT; ++Bit) {
      SCOPED_TRACE(I.Name);
      SCOPED_TRACE(Bit);
      prepare(I, uint64_t(1) << Bit);
      const auto Expected = snapshot();
      const bool Admitted = !(expectedFlags(Flags) & ~Flags);
      const auto Exit = run();
      if (Admitted) {
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        expectPop(Expected);
      } else {
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
            << Exit.Diagnostic;
        EXPECT_EQ(snapshot(), Expected);
        EXPECT_EQ(memory(), Original);
        // A rejected control transition is terminal. Never reuse that CPU.
        initialize();
        ASSERT_FALSE(HasFatalFailure());
      }
    }
}

TEST_P(X64PopFlags, ObserversStopBeforeFlagsAndResumeSavedContext) {
  for (const auto &I : Instructions) {
    prepare(I, IgnoredImage, Data + Page - I.Width / 2);
    const auto Expected = snapshot();
    auto Saved = llvm::cantFail(CPU->saveContext());
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) {
      EXPECT_EQ(snapshot(), Expected);
      CPU->stop();
    };
    ASSERT_EQ(run(std::move(H)).Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    for (bool Restore : {false, true}) {
      if (Restore)
        llvm::cantFail(CPU->restoreContext(*Saved));
      ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
      expectPop(Expected);
    }
  }
}

TEST_P(X64PopFlags, CrossPageFaultPrecedesControlAdmissionAndAllowsRepair) {
  for (const auto &I : Instructions) {
    prepare(I, Trap, Data + Page - I.Width / 2);
    const auto Expected = snapshot();
    llvm::cantFail(CPU->protect(Data + Page, Page, UserAccessible));
    BackendHooks H;
    H.RecoverableFault = [](const BackendFault &) { return true; };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
    const auto Fault = CPU->takeRecoverableFault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->Address, Data + Page);
    EXPECT_EQ(Fault->Size, Width / 2);
    EXPECT_EQ(Fault->Access, BackendAccessKind::Read);
    llvm::cantFail(
        CPU->protect(Data + Page, Page, Read | Write | UserAccessible));
    // Repair memory and the inadmissible flag image before retrying.
    prepare(I, 0, SP);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    expectPop(Expected);
  }
}

TEST_P(X64PopFlags, ReadOnlyAliasesConsumeExactOperandWithoutWrites) {
  llvm::cantFail(
      CPU->mapAlias(Alias + Page, Data, Page, Read | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Alias + 2 * Page, Data + Page, Page,
                               Read | UserAccessible));
  for (const auto &I : Instructions) {
    prepare(I, IgnoredImage, Data + Page - I.Width / 2);
    SP = Alias + 2 * Page - I.Width / 2;
    llvm::cantFail(CPU->setReg(X64Register::SP, SP));
    const auto Expected = snapshot();
    BackendHooks H;
    H.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectPop(Expected);
  }
}

TEST_P(X64PopFlags, UserModeCannotReadSupervisorStack) {
  prepare(Instructions[0], 0, Data + Page - Instructions[0].Width / 2);
  const auto Expected = snapshot();
  llvm::cantFail(CPU->protect(Data + Page, Page, Read));
  const auto Exit = run();
  if (userMode()) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(Exit.Fault->Address, Data + Page);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(memory(), Original);
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectPop(Expected);
  }
}

TEST_P(X64PopFlags, ObserverFailurePreservesCompleteState) {
  prepare(Instructions[0]);
  const auto Expected = snapshot();
  BackendHooks H;
  H.Read = [](uint64_t, uint32_t) {
    throw std::runtime_error(ObserverFailure);
  };
  const auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure) << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), Expected);
  EXPECT_EQ(memory(), Original);
}

TEST_P(X64PopFlags, NoncanonicalAndWrappedStackHaveNoEffects) {
  for (const auto &I : Instructions)
    for (uint64_t Stack : {Noncanonical, UINT64_MAX}) {
      prepare(I, 0, Stack);
      const auto Expected = snapshot();
      BackendHooks H;
      H.RecoverableFault = [](const BackendFault &) { return true; };
      EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::RecoverableFault);
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(memory(), Original);
      ASSERT_TRUE(CPU->takeRecoverableFault());
    }
}

TEST_P(X64PopFlags, ExecutableStackAliasPreservesInstructionBytes) {
  for (const auto &I : Instructions) {
    prepare(I);
    llvm::cantFail(CPU->writeInteger(Code + Page / 2, 0, sizeof(uint64_t)));
    // A native TF workaround must not patch this read-only executable owner.
    llvm::cantFail(CPU->mapAlias(Alias + Page, Code, Page,
                                 Read | Execute | UserAccessible));
    llvm::cantFail(CPU->protect(Code, Page, Read | Execute | UserAccessible));
    SP = Alias + Page + Page / 2;
    llvm::cantFail(CPU->setReg(X64Register::SP, SP));
    const auto Expected = snapshot();
    std::vector<uint8_t> Before(Page), After(Page);
    llvm::cantFail(CPU->snapshotBacking(Code, Before));
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectPop(Expected);
    llvm::cantFail(CPU->snapshotBacking(Code, After));
    EXPECT_EQ(After, Before);
    llvm::cantFail(CPU->unmapAlias(Alias + Page, Page));
    llvm::cantFail(
        CPU->protect(Code, Page, Read | Write | Execute | UserAccessible));
  }
}

TEST_P(X64PopFlags, DeviceStacksRejectWithoutCallbacks) {
  prepare(Instructions[0]);
  unsigned Calls = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [&](uint64_t, uint64_t, bool) {
    ++Calls;
    return llvm::Error::success();
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Calls;
    return 0;
  };
  Device.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Calls;
    return llvm::Error::success();
  };
  auto Mapping = CPU->mapMMIO(DeviceAddress, Page, std::move(Device));
  if (userMode()) {
    EXPECT_TRUE(bool(Mapping));
    llvm::consumeError(std::move(Mapping));
    EXPECT_EQ(Calls, 0u);
    return;
  }
  ASSERT_FALSE(bool(Mapping)) << llvm::toString(std::move(Mapping));
  llvm::cantFail(CPU->setReg(X64Register::SP, DeviceAddress));
  const auto Expected = snapshot();
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
  const auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
      << Exit.Diagnostic;
  EXPECT_EQ(Calls, 0u);
  EXPECT_EQ(snapshot(), Expected);
  EXPECT_EQ(memory(), Original);
}

TEST_P(X64PopFlags, NativeContinuationKeepsEveryInstructionBoundary) {
  prepare(Instructions[0], 0);
  llvm::cantFail(CPU->write(Code, Continuation));
  llvm::cantFail(CPU->setReg(X64Register::CX, Alias));
  llvm::cantFail(CPU->setReg(X64Register::AX, GuestValue));
  const auto Before = snapshot();
  // Stop directly after POPF, before the following native store can run.
  ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
  expectPop(Before);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, sizeof(uint64_t))), 0u);
  unsigned Steps = 0;
  BackendHooks H;
  H.Instruction = [&](uint64_t PC, uint32_t) {
    ++Steps;
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)),
              expectedFlags(Flags));
    if (PC == Code + sizeof(Continuation) - 1)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Code + Length, Timeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Steps, 3u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, sizeof(uint64_t))),
            GuestValue);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(SP, sizeof(uint64_t))),
            expectedFlags(Flags));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SP)), SP);
}

TEST_P(X64PopFlags, LockedFormsRejectBeforeStackObservations) {
  const std::vector<uint8_t> Rejected[] = {
#define NEVERD_POP_FLAGS_REJECTED(Name, ...) {__VA_ARGS__},
#include "X64PopFlagsCases.def"
#undef NEVERD_POP_FLAGS_REJECTED
  };
  for (const auto &Bytes : Rejected) {
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

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PopFlags,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

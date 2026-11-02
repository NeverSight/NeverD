//===- X64FrameExitTests.cpp - Frame exit footprints and atomic state ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_FRAME_EXIT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_FRAME_EXIT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_FRAME_EXIT_ORACLE_BYTES(Name, ...)                              \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_ORACLE_BYTES
#undef NEVERD_FRAME_EXIT_TEXT
#undef NEVERD_FRAME_EXIT_VALUE
struct Instruction {
  const char *Name;
  unsigned Width;
  std::vector<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_FRAME_EXIT_CASE(Name, Width, ...) {#Name, Width, {__VA_ARGS__}},
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_CASE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_FRAME_EXIT_BACKEND(Name, Kind, Contract)                        \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_BACKEND
};
struct Parameter {
  Backend B;
  Instruction I;
  std::string name() const { return std::string(B.Name) + '_' + I.Name; }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends)
    for (const auto &I : Instructions)
      Result.push_back({B, I});
  return Result;
}
using State = std::map<CPURegister, RegisterValue>;
using RAM = std::map<uint64_t, std::vector<uint8_t>>;
void expected(State &S, const Instruction &I, uint64_t Value) {
  const auto BP = S.at(CPURegister::X64BP)[0];
  const auto Mask = UINT64_MAX >> ((WordBytes - I.Width) * ByteBits);
  S[CPURegister::X64BP][0] = (BP & ~Mask) | (Value & Mask);
  S[CPURegister::X64SP][0] = BP + I.Width;
  S[CPURegister::X64PC][0] += I.Bytes.size();
}
class X64FrameExit : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint64_t> Pages;
  const Instruction &instruction() const { return GetParam().I; }
  bool userMode() const {
    return GetParam().B.Contract == ExecutionContract::CheckedUserX64;
  }
  void SetUp() override { initialize(); }
  void initialize(uint64_t BP = Data + Offset, uint64_t Value = Seed) {
    Pages.clear();
    auto B =
        createExecutionBackend(GetParam().B.Kind, GetParam().B.Contract, Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    for (auto Base : {Code, LowData, Data})
      for (unsigned N = 0; N < 2; ++N) {
        const auto A = Base + N * Page;
        llvm::cantFail(
            CPU->map(A, Page, Read | Write | Execute | UserAccessible));
        llvm::cantFail(CPU->write(
            A, std::vector<uint8_t>(Page, Base == Code      ? Nop
                                          : Base == LowData ? LowFill
                                                            : Fill)));
        Pages.push_back(A);
      }
    llvm::cantFail(CPU->write(Code, instruction().Bytes));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    // The old RSP must never participate in LEAVE's implicit read.
    llvm::cantFail(CPU->setReg(X64Register::SP, Noncanonical));
    llvm::cantFail(CPU->setReg(X64Register::BP, BP));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(
        CPU->writeRegister(CPURegister::X64FSBase, {SegmentBase, 0}));
    llvm::cantFail(
        CPU->writeRegister(CPURegister::X64GSBase, {SegmentBase, 0}));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Value - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
    llvm::cantFail(CPU->writeInteger(BP, Value, instruction().Width));
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
  RAM memory(bool Observing = false) {
    RAM Result;
    for (auto A : Pages) {
      auto &Bytes = Result[A];
      Bytes.resize(Page);
      if (Observing)
        llvm::cantFail(CPU->addressSpace()->read(A, Bytes));
      else
        llvm::cantFail(CPU->snapshotBacking(A, Bytes));
    }
    return Result;
  }
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t Size) {
      if (PC == Code)
        EXPECT_EQ(Size, instruction().Bytes.size());
      else {
        EXPECT_EQ(PC, Code + instruction().Bytes.size());
        CPU->stop();
      }
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};
TEST_P(X64FrameExit, WidthsPrefixesAndFullFrameAddressPreserveCompleteState) {
  for (auto BP : {LowData + Offset, Data + Offset})
    for (auto Value : {uint64_t(0), Seed, UINT64_MAX}) {
      initialize(BP, Value);
      if (HasFatalFailure() || IsSkipped())
        return;
      auto Before = snapshot();
      const auto Original = memory();
      unsigned Reads = 0;
      BackendHooks H;
      H.Read = [&](uint64_t A, unsigned Size) {
        ++Reads;
        EXPECT_EQ(A, BP);
        EXPECT_EQ(Size, instruction().Width);
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(true), Original);
      };
      H.Write = [](uint64_t, unsigned, uint64_t) { ADD_FAILURE(); };
      const auto Exit = run(std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(Reads, 1u);
      expected(Before, instruction(), Value);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), Original);
    }
}
TEST_P(X64FrameExit, ReadStopsAndFailuresPreserveFrameAndStack) {
  const auto Before = snapshot();
  const auto Original = memory();
  for (bool Throw : {false, true}) {
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Before.at(CPURegister::X64BP)[0]);
      EXPECT_EQ(Size, instruction().Width);
      EXPECT_EQ(snapshot(), Before);
      ++Reads;
      if (Throw)
        throw std::runtime_error(ObserverFailure);
      CPU->stop();
    };
    const auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                               : ExecutionExitKind::Stopped)
        << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
  }
}
TEST_P(X64FrameExit, ReadOnlySplitFramesAndAliasesConsumeExactWidth) {
  for (unsigned Split = 1; Split < instruction().Width; ++Split) {
    initialize(Data + Page - Split);
    if (HasFatalFailure() || IsSkipped())
      return;
    for (unsigned N = 0; N < 2; ++N) {
      llvm::cantFail(CPU->mapAlias(Alias + N * Page, Data + N * Page, Page,
                                   Read | UserAccessible));
      Pages.push_back(Alias + N * Page);
    }
    llvm::cantFail(CPU->setReg(X64Register::BP, Alias + Page - Split));
    auto Before = snapshot();
    const auto Original = memory();
    BackendHooks H;
    H.Write = [](uint64_t, unsigned, uint64_t) { ADD_FAILURE(); };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expected(Before, instruction(), Seed);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
  }
}
TEST_P(X64FrameExit, MissingOrDeniedSuffixPreservesStateAndCanResume) {
  for (unsigned Split = 1; Split < instruction().Width; ++Split)
    for (bool Missing : {false, true}) {
      initialize(Data + Page - Split);
      if (HasFatalFailure() || IsSkipped())
        return;
      llvm::cantFail(CPU->mapAlias(Alias, Data + Page, Page,
                                   Read | Write | UserAccessible));
      Pages.push_back(Alias);
      Pages.erase(std::find(Pages.begin(), Pages.end(), Data + Page));
      const auto Before = snapshot();
      const auto Original = memory();
      if (Missing)
        llvm::cantFail(CPU->addressSpace()->unmap(Data + Page, Page));
      else
        llvm::cantFail(CPU->protect(Data + Page, Page, Write | UserAccessible));
      BackendHooks H;
      H.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Exit = run(std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, Missing ? BackendFaultKind::UnmappedMemory
                                          : BackendFaultKind::Protection);
      EXPECT_EQ(Exit.Fault->Address, Data + Page);
      EXPECT_EQ(Exit.Fault->Size, instruction().Width - Split);
      EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), Original);
      EXPECT_TRUE(CPU->takeRecoverableFault());
      EXPECT_FALSE(CPU->takeRecoverableFault());
      if (Missing)
        llvm::cantFail(
            CPU->mapAlias(Data + Page, Alias, Page, Read | UserAccessible));
      else
        llvm::cantFail(CPU->protect(Data + Page, Page, Read | UserAccessible));
      auto After = Before;
      expected(After, instruction(), Seed);
      const auto Retried = run();
      ASSERT_EQ(Retried.Kind, ExecutionExitKind::Stopped) << Retried.Diagnostic;
      EXPECT_EQ(snapshot(), After);
      EXPECT_EQ(memory(), Original);
    }
}
TEST_P(X64FrameExit, SupervisorFramePermissionsFollowGuestPrivilege) {
  initialize(Data + Page - 1);
  if (HasFatalFailure() || IsSkipped())
    return;
  auto Before = snapshot();
  const auto Original = memory();
  llvm::cantFail(CPU->protect(Data + Page, Page, Read));
  const auto Exit = run();
  if (userMode()) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(Exit.Fault->Address, Data + Page);
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expected(Before, instruction(), Seed);
  }
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), Original);
}
TEST_P(X64FrameExit, InvalidFrameAddressesPreserveOldStackAndContext) {
  for (auto BP : {Noncanonical, Noncanonical - 1, UINT64_MAX}) {
    llvm::cantFail(CPU->setReg(X64Register::BP, BP));
    const auto Before = snapshot();
    const auto Original = memory();
    BackendHooks H;
    H.RecoverableFault = [](const BackendFault &) { return true; };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
    EXPECT_TRUE(CPU->takeRecoverableFault());
  }
}
TEST_P(X64FrameExit, DeviceFramesRejectBeforeObservationsOrCallbacks) {
  for (bool Split : {false, true}) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    unsigned Calls = 0, Reads = 0;
    GuestMMIOCallbacks Device;
    Device.Validate = [&](uint64_t, unsigned, bool) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Calls;
      return Seed;
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
    if (Split)
      llvm::cantFail(CPU->mapAlias(DeviceAddress - Page, Data, Page,
                                   Read | UserAccessible));
    llvm::cantFail(
        CPU->setReg(X64Register::BP, DeviceAddress - (Split ? 1 : 0)));
    const auto Before = snapshot();
    const auto Original = memory();
    BackendHooks H;
    H.Read = [&](uint64_t, unsigned) { ++Reads; };
    H.Write = [](uint64_t, unsigned, uint64_t) { ADD_FAILURE(); };
    const auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
    EXPECT_EQ(Calls, 0u);
    EXPECT_EQ(Reads, 0u);
  }
}
TEST_P(X64FrameExit, RejectedPrefixesPublishNoObservationsOrEffects) {
  constexpr uint8_t Prefixes[] = {
#define NEVERD_FRAME_EXIT_REJECTED_PREFIX(Value) Value,
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_REJECTED_PREFIX
  };
  for (auto Prefix : Prefixes) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    auto Bytes = instruction().Bytes;
    Bytes.insert(Bytes.begin(), Prefix);
    llvm::cantFail(CPU->write(Code, Bytes));
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
  }
}
TEST_P(X64FrameExit, SavedContextReplaysAgainstCurrentFrameBytes) {
  const auto Before = snapshot();
  auto Saved = llvm::cantFail(CPU->saveContext());
  for (auto Value : {Seed, ~Seed}) {
    llvm::cantFail(CPU->restoreContext(*Saved));
    EXPECT_EQ(snapshot(), Before);
    llvm::cantFail(
        CPU->writeInteger(Data + Offset, Value, instruction().Width));
    const auto Original = memory();
    auto After = Before;
    expected(After, instruction(), Value);
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), After);
    EXPECT_EQ(memory(), Original);
  }
}
TEST(X64FrameExitOracle, OriginalHostInstructionsDetermineWidthsAndFrameUse) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned Executed = 0;
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    std::vector<uint8_t> Program;
    auto Append = [&](llvm::ArrayRef<uint8_t> Bytes) {
      Program.insert(Program.end(), Bytes.begin(), Bytes.end());
    };
    Append(OraclePrefix);
#ifdef _WIN32
    Append(Win64Argument);
#else
    Append(SysVArgument);
#endif
    Append(OracleLoad);
    Append(I.Bytes);
    Append(OracleSave);
    std::error_code EC;
    auto Block = llvm::sys::Memory::allocateMappedMemory(
        Page, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
        EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto Release = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
    std::memcpy(Block.base(), Program.data(), Program.size());
    EC = llvm::sys::Memory::protectMappedMemory(
        Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Program.size());
    auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
    for (auto Value : {uint64_t(0), Seed, UINT64_MAX}) {
      std::array<uint64_t, OracleWords> Packet{};
      State S{{CPURegister::X64BP, {reinterpret_cast<uintptr_t>(&Value), 0}},
              {CPURegister::X64SP, {Noncanonical, 0}},
              {CPURegister::X64FLAGS, {Flags, 0}},
              {CPURegister::X64PC, {0, 0}}};
#define NEVERD_FRAME_EXIT_ORACLE_REGISTER(Name, N)                             \
  Packet[N] = S.at(CPURegister::X64##Name)[0];
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_ORACLE_REGISTER
      expected(S, I, Value);
      const auto Before = Value;
      Execute(Packet.data());
      ++Executed;
#define NEVERD_FRAME_EXIT_ORACLE_REGISTER(Name, N)                             \
  EXPECT_EQ(Packet[N + OracleFields], S.at(CPURegister::X64##Name)[0]);
#include "X64FrameExitCases.def"
#undef NEVERD_FRAME_EXIT_ORACLE_REGISTER
      EXPECT_EQ(Value, Before);
    }
  }
  EXPECT_EQ(Executed, std::size(Instructions) * 3);
  std::cout << OracleExecuted << Executed << '\n';
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64FrameExit,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation

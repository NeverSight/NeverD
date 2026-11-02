//===- X64LoopTests.cpp - Counted branches across CPU transports
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_LOOP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LOOP_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_LOOP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64LoopCases.def"
#undef NEVERD_LOOP_BYTES
#undef NEVERD_LOOP_TEXT
#undef NEVERD_LOOP_VALUE
constexpr uint64_t Counts[] = {
#define NEVERD_LOOP_COUNT(Value) Value,
#include "X64LoopCases.def"
#undef NEVERD_LOOP_COUNT
};
constexpr uint64_t Flags[] = {
#define NEVERD_LOOP_FLAGS(Value) Value,
#include "X64LoopCases.def"
#undef NEVERD_LOOP_FLAGS
};
constexpr int Displacements[] = {
#define NEVERD_LOOP_DISPLACEMENT(Value) Value,
#include "X64LoopCases.def"
#undef NEVERD_LOOP_DISPLACEMENT
};
struct Loop {
  const char *Name;
  int Condition;
  bool Counter32, Operand16;
  std::vector<uint8_t> Bytes;
};
const Loop Loops[] = {
#define NEVERD_LOOP_CASE(Name, Condition, Counter32, Operand16, ...)           \
  {#Name, Condition, Counter32, Operand16, {__VA_ARGS__}},
#include "X64LoopCases.def"
#undef NEVERD_LOOP_CASE
};
std::vector<uint8_t> encoding(const Loop &L, int Offset) {
  auto Bytes = L.Bytes;
  Bytes.push_back(uint8_t(Offset));
  return Bytes;
}
uint64_t remaining(const Loop &L, uint64_t Count) {
  return L.Counter32 ? uint32_t(Count - 1) : Count - 1;
}
bool taken(const Loop &L, uint64_t Count, uint64_t Flags) {
  return remaining(L, Count) &&
         (!L.Condition || bool(Flags & ZeroFlag) == (L.Condition > 0));
}
bool nativeAMD() {
#if defined(_MSC_VER) && defined(_M_X64)
  int Registers[4];
  __cpuid(Registers, 0);
  return uint32_t(Registers[1]) == AMDVendorEBX &&
         uint32_t(Registers[3]) == AMDVendorEDX &&
         uint32_t(Registers[2]) == AMDVendorECX;
#elif defined(__x86_64__)
  unsigned AX, BX, CX, DX;
  __cpuid(0, AX, BX, CX, DX);
  return BX == AMDVendorEBX && DX == AMDVendorEDX && CX == AMDVendorECX;
#else
  return false;
#endif
}

TEST(X64LoopOracle, CountsFlagsAndBranchesMatchOriginalHostInstructions) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned Executed = 0, GuestOnly = 0;
  for (const auto &L : Loops) {
    std::vector<uint8_t> Bytes;
    auto Append = [&](llvm::ArrayRef<uint8_t> Part) {
      Bytes.insert(Bytes.end(), Part.begin(), Part.end());
    };
    Append(OraclePrefix);
#ifdef _WIN32
    Append(Win64Argument);
#else
    Append(SysVArgument);
#endif
    Append(OracleLoad);
    Append(encoding(L, sizeof(OracleFalse) + sizeof(OracleSkipTrue)));
    Append(OracleFalse);
    Append(OracleSkipTrue);
    Append(OracleTrue);
    Append(OracleSave);
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
    for (uint64_t Count : Counts)
      for (uint64_t Before : Flags) {
        SCOPED_TRACE(L.Name);
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Before);
        // Host operating systems reserve the low 64 KiB. AMD's taken 66H
        // branches target that range, so execute those forms in the mapped
        // guest matrix below instead of jumping into unmapped host memory.
        if (nativeAMD() && L.Operand16 && taken(L, Count, Before)) {
          ++GuestOnly;
          continue;
        }
        std::array<uint64_t, 5> Packet{Count, Before};
        Execute(Packet.data());
        ++Executed;
        ASSERT_EQ(Packet[2], remaining(L, Count));
        ASSERT_EQ(Packet[3], Packet[1]);
        ASSERT_EQ(Packet[4], taken(L, Count, Packet[1]));
      }
  }
  std::cout << OracleExecuted << Executed << OracleGuestOnly << GuestOnly
            << '\n';
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_LOOP_BACKEND(Name, Kind, Contract)                              \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64LoopCases.def"
#undef NEVERD_LOOP_BACKEND
};
struct Parameter {
  Backend B;
  Loop L;
  std::string name() const { return std::string(B.Name) + '_' + L.Name; }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends)
    for (const auto &L : Loops)
      Result.push_back({B, L});
  return Result;
}
class X64Loop : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Entry = Code + EntryOffset;
  unsigned Length = 0;
  const Loop &loop() const { return GetParam().L; }
  bool narrowTarget() const {
    // The software transport uses its configured default Intel model;
    // hardware transports execute the actual host processor's instruction.
    return GetParam().B.Kind != ExecutionBackendKind::Unicorn && nativeAMD() &&
           loop().Operand16;
  }
  uint64_t target(uint64_t Count, uint64_t Before, int Offset) const {
    const uint64_t Fallthrough = Entry + Length;
    if (!taken(loop(), Count, Before))
      return Fallthrough;
    const uint64_t Target = Fallthrough + Offset;
    return narrowTarget() ? uint16_t(Target) : Target;
  }
  void SetUp() override { initialize(); }
  void initialize() {
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
    const std::vector<uint8_t> Padding(Page, Nop);
    for (uint64_t Address : {LowCode, LowCode + Page, Code, HighCode}) {
      llvm::cantFail(
          CPU->map(Address, Page, Read | Write | Execute | UserAccessible));
      llvm::cantFail(CPU->write(Address, Padding));
    }
    llvm::cantFail(CPU->map(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(CPU->write(Data, std::vector<uint8_t>(Page, Fill)));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Source - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
  }
  void prepare(uint64_t Count = FiniteCount, uint64_t Before = SetFlags,
               int Offset = Displacement) {
    const auto Bytes = encoding(loop(), Offset);
    Length = Bytes.size();
    llvm::cantFail(CPU->write(Entry, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::CX, Count));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Before));
    llvm::cantFail(CPU->setReg(X64Register::PC, Entry));
  }
  uint64_t activeFlags() const {
    return loop().Condition < 0 ? SetFlags & ~ZeroFlag : SetFlags;
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
    std::vector<uint8_t> Bytes(Page);
    if (auto E = CPU->addressSpace()->read(Data, Bytes))
      ADD_FAILURE() << llvm::toString(std::move(E));
    return Bytes;
  }
  ExecutionExit run(BackendHooks H = {}) {
    unsigned Visits = 0;
    if (!H.Instruction)
      H.Instruction = [&](uint64_t, uint32_t) {
        if (++Visits == 2)
          CPU->stop();
      };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
  }
};

TEST_P(X64Loop, CountsFlagsAndSignedTargetsPreserveOtherState) {
  const auto RAM = memory();
  for (uint64_t Base : {Code, HighCode})
    for (int Offset : Displacements)
      for (uint64_t Count : Counts)
        for (uint64_t Before : Flags) {
          SCOPED_TRACE(Base);
          SCOPED_TRACE(Offset);
          SCOPED_TRACE(Count);
          SCOPED_TRACE(Before);
          Entry = Base + EntryOffset;
          prepare(Count, Before, Offset);
          auto State = snapshot();
          unsigned Reads = 0, Writes = 0;
          BackendHooks H;
          H.Read = [&](uint64_t, uint32_t) { ++Reads; };
          H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
          const auto Exit = run(H);
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          State[CPURegister::X64CX][0] = remaining(loop(), Count);
          State[CPURegister::X64PC][0] = target(Count, Before, Offset);
          ASSERT_EQ(snapshot(), State);
          ASSERT_EQ(Reads, 0u);
          ASSERT_EQ(Writes, 0u);
        }
  EXPECT_EQ(memory(), RAM);
}
TEST_P(X64Loop, InstructionStopsAndFailuresPrecedeCounterEffects) {
  for (bool Throw : {false, true}) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    prepare(FiniteCount, activeFlags());
    const auto State = snapshot();
    const auto RAM = memory();
    unsigned Visits = 0;
    BackendHooks H;
    H.Instruction = [&](uint64_t PC, uint32_t Size) {
      EXPECT_EQ(PC, Entry);
      EXPECT_EQ(Size, Length);
      ++Visits;
      if (Throw)
        throw std::runtime_error(ObserverFailure);
      CPU->stop();
    };
    const auto Exit = run(H);
    EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                               : ExecutionExitKind::Stopped)
        << Exit.Diagnostic;
    EXPECT_EQ(Visits, 1u);
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(memory(), RAM);
  }
}
TEST_P(X64Loop, CrossPageDecodeRequiresOnlyCompleteExecutableBytes) {
  enum class Suffix { Executable, Absent, NoExecute };
  for (unsigned Split = 1; Split <= loop().Bytes.size(); ++Split)
    for (auto S : {Suffix::Executable, Suffix::Absent, Suffix::NoExecute}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      llvm::cantFail(
          CPU->map(Code + Page, Page, Read | Write | Execute | UserAccessible));
      llvm::cantFail(CPU->write(Code + Page, std::vector<uint8_t>(Page, Nop)));
      Entry = Code + Page - Split;
      prepare(FiniteCount, activeFlags());
      if (S == Suffix::Absent)
        llvm::cantFail(CPU->addressSpace()->unmap(Code + Page, Page));
      else if (S == Suffix::NoExecute)
        llvm::cantFail(
            CPU->protect(Code + Page, Page, Read | Write | UserAccessible));
      auto State = snapshot();
      const auto RAM = memory();
      const auto Exit = run();
      if (S == Suffix::Executable) {
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        State[CPURegister::X64CX][0] = FiniteCount - 1;
        State[CPURegister::X64PC][0] =
            target(FiniteCount, activeFlags(), Displacement);
      } else {
        // The shared decoder reports incomplete instructions as unsupported.
        // No counter, flags or PC effects may escape that decode rejection.
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
            << Exit.Diagnostic;
        ASSERT_TRUE(Exit.Fault);
        EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::InvalidInstruction);
      }
      EXPECT_EQ(snapshot(), State);
      EXPECT_EQ(memory(), RAM);
    }
}
TEST_P(X64Loop, TargetFetchFaultRetainsTheRetiredBranch) {
  for (bool NoExecute : {false, true}) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    Entry = Code + Page - loop().Bytes.size() - 1;
    prepare(FiniteCount, activeFlags());
    const uint64_t Target = target(FiniteCount, activeFlags(), Displacement);
    const uint64_t TargetPage = Target & ~(Page - 1);
    if (narrowTarget())
      llvm::cantFail(CPU->addressSpace()->unmap(TargetPage, Page));
    if (NoExecute)
      llvm::cantFail(CPU->map(TargetPage, Page, Read | Write | UserAccessible));
    auto State = snapshot();
    const auto RAM = memory();
    const auto Exit = run();
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    State[CPURegister::X64CX][0] = FiniteCount - 1;
    State[CPURegister::X64PC][0] = Target;
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(memory(), RAM);
  }
}
TEST_P(X64Loop, ContextRestoreAndFiniteSelfLoopsRetainCounterAndFlags) {
  if (narrowTarget())
    Entry = LowCode + EntryOffset;
  prepare(FiniteCount, activeFlags(), -int(loop().Bytes.size() + 1));
  auto State = snapshot();
  const auto RAM = memory();
  auto Saved = llvm::cantFail(CPU->saveContext());
  ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
  State[CPURegister::X64CX][0] = FiniteCount - 1;
  EXPECT_EQ(snapshot(), State);
  for (bool Restore : {false, true}) {
    if (Restore)
      llvm::cantFail(CPU->restoreContext(*Saved));
    std::vector<uint64_t> Observed;
    BackendHooks H;
    H.Instruction = [&](uint64_t PC, uint32_t) {
      Observed.push_back(llvm::cantFail(CPU->reg(X64Register::CX)));
      if (PC == Entry + Length)
        CPU->stop();
      else
        EXPECT_EQ(PC, Entry);
    };
    const auto Exit = run(H);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    std::vector<uint64_t> Expected;
    for (uint64_t Count = Restore ? FiniteCount : FiniteCount - 1;; --Count) {
      Expected.push_back(Count);
      if (!Count)
        break;
    }
    EXPECT_EQ(Observed, Expected);
    State[CPURegister::X64CX][0] = 0;
    State[CPURegister::X64PC][0] = Entry + Length;
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(memory(), RAM);
  }
}
TEST_P(X64Loop, UnsupportedPrefixesRejectBeforeEffects) {
  for (uint8_t Prefix : {Lock, Rep, Repne}) {
    SCOPED_TRACE(unsigned(Prefix));
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    prepare(FiniteCount, activeFlags());
    auto Bytes = encoding(loop(), Displacement);
    Bytes.insert(Bytes.begin(), Prefix);
    llvm::cantFail(CPU->write(Entry, Bytes));
    const auto State = snapshot();
    const auto RAM = memory();
    const auto Exit = run();
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(memory(), RAM);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Loop,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation

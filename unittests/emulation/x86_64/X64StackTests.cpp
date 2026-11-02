//===- X64StackTests.cpp - Stack widths, addressing and transactions ----===//
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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_STACK_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_STACK_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_STACK_ORACLE_BYTES(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StackCases.def"
#undef NEVERD_STACK_ORACLE_BYTES
#undef NEVERD_STACK_TEXT
#undef NEVERD_STACK_VALUE
enum class Operand { Register, Immediate, Memory };
enum class Address {
  Base,
  Stack,
  Previous,
  Overlap,
  Indexed,
  Extended,
  Address32,
  StackAddress32,
  IP,
  FS,
  GS
};
struct Instruction {
  const char *Name;
  bool Push;
  unsigned Width;
  Operand Kind;
  CPURegister Register;
  Address Location;
  int64_t Immediate;
  std::vector<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_STACK_CASE(Name, Push, Width, Kind, Register, Location, Imm,    \
                          ...)                                                 \
  {#Name,                                                                      \
   Push,                                                                       \
   Width,                                                                      \
   Operand::Kind,                                                              \
   CPURegister::Register,                                                      \
   Address::Location,                                                          \
   Imm,                                                                        \
   {__VA_ARGS__}},
#include "X64StackCases.def"
#undef NEVERD_STACK_CASE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_STACK_BACKEND(Name, Kind, Contract)                             \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64StackCases.def"
#undef NEVERD_STACK_BACKEND
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
uint64_t mask(unsigned Width) {
  return UINT64_MAX >> ((WordBytes - Width) * ByteBits);
}
struct Expectation {
  const Instruction &I;
  uint64_t Entry;
  bool Aliased = false;
  uint64_t explicitAddress(const State &S) const {
    const uint64_t SP = S.at(CPURegister::X64SP)[0] + (I.Push ? 0 : I.Width);
    const uint64_t BX = S.at(CPURegister::X64BX)[0];
    switch (I.Location) {
    case Address::Base:
      return BX;
    case Address::Stack:
      return SP;
    case Address::Previous:
      return SP - I.Width;
    case Address::Overlap:
      return SP - 1;
    case Address::Indexed:
      return SP + S.at(CPURegister::X64CX)[0] * IndexScale + IndexOffset;
    case Address::Extended:
      return S.at(CPURegister::X64R12)[0];
    case Address::Address32:
      return uint32_t(BX);
    case Address::StackAddress32:
      return uint32_t(SP);
    case Address::IP:
      return Entry + I.Bytes.size() + IPOffset;
    case Address::FS:
      return BX + S.at(CPURegister::X64FSBase)[0];
    case Address::GS:
      return BX + S.at(CPURegister::X64GSBase)[0];
    }
    std::abort();
  }
  uint64_t readAddress(const State &S) const {
    return I.Push ? explicitAddress(S) : S.at(CPURegister::X64SP)[0];
  }
  uint64_t writeAddress(const State &S) const {
    return I.Push ? S.at(CPURegister::X64SP)[0] - I.Width : explicitAddress(S);
  }
  uint64_t normalized(uint64_t A) const {
    return Aliased && A >= Alias && A < Alias + 2 * Page ? A - Alias + Stack
                                                         : A;
  }
  uint64_t value(const State &S, const RAM &M) const {
    if (I.Push && I.Kind == Operand::Register)
      return S.at(I.Register)[0] & mask(I.Width);
    if (I.Push && I.Kind == Operand::Immediate)
      return uint64_t(I.Immediate) & mask(I.Width);
    uint64_t V = 0;
    for (unsigned N = 0; N < I.Width; ++N) {
      const auto A = readAddress(S) + N;
      V |= uint64_t(M.at(A & ~(Page - 1))[A % Page]) << (N * ByteBits);
    }
    return V;
  }
  void expected(State &S, RAM &M) const {
    const auto V = value(S, M);
    if (I.Push || I.Kind == Operand::Memory) {
      const auto Dest = writeAddress(S);
      for (unsigned N = 0; N < I.Width; ++N)
        for (auto &[Base, Bytes] : M)
          if (const auto A = normalized(Dest + N), B = normalized(Base);
              A >= B && A - B < Page)
            Bytes[A - B] = uint8_t(V >> (N * ByteBits));
    }
    S[CPURegister::X64SP][0] += I.Push ? -uint64_t(I.Width) : I.Width;
    if (!I.Push && I.Kind == Operand::Register)
      S[I.Register][0] = (S[I.Register][0] & ~mask(I.Width)) | V;
    S[CPURegister::X64PC][0] += I.Bytes.size();
  }
};
class X64Stack : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint64_t> Pages;
  uint64_t Entry = Code + Offset;
  bool Aliased = false;
  const Instruction &instruction() const { return GetParam().I; }
  void SetUp() override { initialize(); }
  void initialize(uint64_t Input = Seed, bool UpperStack = false) {
    Pages.clear();
    Aliased = false;
    Entry = Code + Offset;
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
    for (uint64_t Base : {Code, Data, TLS, Stack, Stack + High})
      for (unsigned N = 0; N < 2; ++N) {
        const auto A = Base + N * Page;
        llvm::cantFail(
            CPU->map(A, Page, Read | Write | Execute | UserAccessible));
        llvm::cantFail(CPU->write(
            A, std::vector<uint8_t>(Page, Base == Code ? Nop : Fill)));
        Pages.push_back(A);
      }
    llvm::cantFail(CPU->write(Entry, instruction().Bytes));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Input + R));
    llvm::cantFail(
        CPU->setReg(X64Register::SP, Stack + Offset + (UpperStack ? High : 0)));
    llvm::cantFail(CPU->setReg(
        X64Register::BX,
        Data + Offset +
            (UpperStack && instruction().Location == Address::Address32 ? High
                                                                        : 0)));
    llvm::cantFail(CPU->setReg(X64Register::R12, Data + Offset));
    llvm::cantFail(CPU->setReg(X64Register::CX, Index));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FSBase, {TLS - Data, 0}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64GSBase, {TLS - Data, 0}));
    llvm::cantFail(CPU->setReg(X64Register::PC, Entry));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->setReg(X64Register::FPCW, FPCW));
    llvm::cantFail(CPU->setReg(X64Register::FPSW, FPSW));
    for (unsigned N = 0; N < VectorCount; ++N)
      llvm::cantFail(CPU->setXmm(N, {Seed + N, Input - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
    if (!instruction().Push || instruction().Kind == Operand::Memory)
      llvm::cantFail(CPU->writeInteger(readAddress(snapshot()), Input,
                                       instruction().Width));
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
  Expectation expectation() const { return {instruction(), Entry, Aliased}; }
  uint64_t readAddress(const State &S) const {
    return expectation().readAddress(S);
  }
  uint64_t writeAddress(const State &S) const {
    return expectation().writeAddress(S);
  }
  uint64_t value(const State &S, const RAM &M) const {
    return expectation().value(S, M);
  }
  void expected(State &S, RAM &M) const { expectation().expected(S, M); }
  void crossPage(bool AtWrite, unsigned Split) {
    const auto &I = instruction();
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FSBase, {0, 0}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64GSBase, {0, 0}));
    const auto S = snapshot();
    const auto A = AtWrite ? writeAddress(S) : readAddress(S);
    const uint64_t Delta = (A & ~(Page - 1)) + Page - Split - A;
    X64Register Base = X64Register::SP;
    if (AtWrite != I.Push) {
      switch (I.Location) {
      case Address::Base:
      case Address::Address32:
      case Address::FS:
      case Address::GS:
        Base = X64Register::BX;
        break;
      case Address::Extended:
        Base = X64Register::R12;
        break;
      case Address::IP:
        Entry += Delta;
        llvm::cantFail(CPU->setReg(X64Register::PC, Entry));
        llvm::cantFail(CPU->write(Entry, I.Bytes));
        return;
      default:
        break;
      }
    }
    llvm::cantFail(CPU->setReg(Base, llvm::cantFail(CPU->reg(Base)) + Delta));
  }
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t Size) {
      if (PC == Entry)
        EXPECT_EQ(Size, instruction().Bytes.size());
      else
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
  }
};
TEST_P(X64Stack, WidthsAddressesAndCompleteStateMatch) {
  for (uint64_t Input : {uint64_t(0), Seed, UINT64_MAX})
    for (bool Upper : {false, true}) {
      initialize(Input, Upper);
      if (HasFatalFailure() || IsSkipped())
        return;
      auto S = snapshot();
      auto M = memory();
      expected(S, M);
      const auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(snapshot(), S);
      EXPECT_EQ(memory(), M);
    }
}
TEST_P(X64Stack, OrderedObserversSeeOriginalStateAndExactValues) {
  const auto Before = snapshot();
  const auto Original = memory();
  const auto &I = instruction();
  const bool Reads = !I.Push || I.Kind == Operand::Memory;
  const bool Writes = I.Push || I.Kind == Operand::Memory;
  unsigned ReadCount = 0, WriteCount = 0;
  BackendHooks H;
  H.Read = [&](uint64_t A, uint32_t Size) {
    EXPECT_TRUE(Reads);
    EXPECT_EQ(A, readAddress(Before));
    EXPECT_EQ(Size, I.Width);
    EXPECT_EQ(WriteCount, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(true), Original);
    ++ReadCount;
  };
  H.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
    EXPECT_TRUE(Writes);
    EXPECT_EQ(A, writeAddress(Before));
    EXPECT_EQ(Size, I.Width);
    EXPECT_EQ(V, value(Before, Original));
    EXPECT_EQ(ReadCount, unsigned(Reads));
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(true), Original);
    ++WriteCount;
  };
  const auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(ReadCount, unsigned(Reads));
  EXPECT_EQ(WriteCount, unsigned(Writes));
  auto S = Before;
  auto M = Original;
  expected(S, M);
  EXPECT_EQ(snapshot(), S);
  EXPECT_EQ(memory(), M);
}
TEST_P(X64Stack, StopsAndObserverFailuresPublishNoCPUOrRAM) {
  const auto &I = instruction();
  for (bool AtWrite : {false, true}) {
    if (AtWrite ? (!I.Push && I.Kind != Operand::Memory)
                : (I.Push && I.Kind != Operand::Memory))
      continue;
    for (bool Throw : {false, true}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      const auto S = snapshot();
      const auto M = memory();
      unsigned Visits = 0;
      auto Observe = [&] {
        EXPECT_EQ(snapshot(), S);
        EXPECT_EQ(memory(true), M);
        ++Visits;
        if (Throw)
          throw std::runtime_error(ObserverFailure);
        CPU->stop();
      };
      BackendHooks H;
      if (AtWrite)
        H.Write = [&](uint64_t, uint32_t, uint64_t) { Observe(); };
      else
        H.Read = [&](uint64_t, uint32_t) { Observe(); };
      const auto Exit = run(std::move(H));
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Visits, 1u);
      EXPECT_EQ(snapshot(), S);
      EXPECT_EQ(memory(), M);
    }
  }
}
TEST_P(X64Stack, DeniedOperandOrStackPreservesCompleteState) {
  const auto &I = instruction();
  for (bool AtWrite : {false, true}) {
    if (AtWrite ? (!I.Push && I.Kind != Operand::Memory)
                : (I.Push && I.Kind != Operand::Memory))
      continue;
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    const auto S = snapshot();
    const auto M = memory();
    const uint64_t A = AtWrite ? writeAddress(S) : readAddress(S);
    const unsigned Permissions =
        (AtWrite ? Read : Write) | Execute | UserAccessible;
    llvm::cantFail(CPU->protect(A & ~(Page - 1), Page, Permissions));
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(snapshot(), S);
    EXPECT_EQ(memory(), M);
  }
}
TEST_P(X64Stack, SplitPagesAndPhysicalAliasesRetainExactFootprints) {
  const auto &I = instruction();
  for (unsigned Split = 1; Split < I.Width; ++Split)
    for (bool Shared : {false, true})
      for (bool StackWrite : {false, true}) {
        if (StackWrite && !I.Push)
          continue;
        initialize();
        if (HasFatalFailure() || IsSkipped())
          return;
        if (Shared) {
          for (unsigned N = 0; N < 2; ++N) {
            llvm::cantFail(CPU->mapAlias(Alias + N * Page, Stack + N * Page,
                                         Page, Read | Write | UserAccessible));
            Pages.push_back(Alias + N * Page);
          }
          Aliased = true;
        }
        llvm::cantFail(
            CPU->setReg(X64Register::SP,
                        Stack + Page - Split + (StackWrite ? I.Width : 0)));
        const uint64_t Base = Shared ? Alias : Data;
        llvm::cantFail(CPU->setReg(X64Register::BX, Base + Page - Split));
        llvm::cantFail(CPU->setReg(X64Register::R12, Base + Page - Split));
        llvm::cantFail(CPU->writeRegister(CPURegister::X64FSBase, {0, 0}));
        llvm::cantFail(CPU->writeRegister(CPURegister::X64GSBase, {0, 0}));
        if (!I.Push || I.Kind == Operand::Memory)
          llvm::cantFail(
              CPU->writeInteger(readAddress(snapshot()), Seed, I.Width));
        auto S = snapshot();
        auto M = memory();
        expected(S, M);
        const auto Exit = run();
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(snapshot(), S);
        EXPECT_EQ(memory(), M);
      }
}
TEST_P(X64Stack, CrossPageFaultAndRepairPreserveOneAtomicTransfer) {
  const auto &I = instruction();
  for (bool AtWrite : {false, true}) {
    if (AtWrite ? (!I.Push && I.Kind != Operand::Memory)
                : (I.Push && I.Kind != Operand::Memory))
      continue;
    for (unsigned Split = 1; Split < I.Width; ++Split)
      for (bool Missing : {false, true}) {
        SCOPED_TRACE(AtWrite);
        SCOPED_TRACE(Split);
        SCOPED_TRACE(Missing);
        initialize();
        if (HasFatalFailure() || IsSkipped())
          return;
        crossPage(AtWrite, Split);
        const auto S = snapshot();
        const auto A = AtWrite ? writeAddress(S) : readAddress(S);
        ASSERT_EQ(A % Page, Page - Split);
        if (!I.Push || I.Kind == Operand::Memory)
          llvm::cantFail(CPU->writeInteger(readAddress(S), Seed, I.Width));
        const uint64_t Suffix = (A & ~(Page - 1)) + Page;
        // Keep a physical alias so even unmapped bytes can be checked for a
        // speculative prefix or suffix store and restored without copying.
        llvm::cantFail(
            CPU->mapAlias(Alias, Suffix, Page, Read | Write | UserAccessible));
        Pages.push_back(Alias);
        Pages.erase(std::find(Pages.begin(), Pages.end(), Suffix));
        const auto M = memory();
        if (Missing)
          llvm::cantFail(CPU->addressSpace()->unmap(Suffix, Page));
        else
          llvm::cantFail(CPU->protect(Suffix, Page,
                                      (AtWrite ? Read : Write) | Execute |
                                          UserAccessible));
        BackendHooks H;
        H.RecoverableFault = [](const BackendFault &) { return true; };
        const auto Exit = run(std::move(H));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(Exit.Fault);
        EXPECT_EQ(Exit.Fault->Kind, Missing ? BackendFaultKind::UnmappedMemory
                                            : BackendFaultKind::Protection);
        // A missing suffix may also contain the source of PUSH [RSP]. The
        // source read is admitted first, so its first missing byte owns the
        // fault even when the test positioned the destination across pages.
        const bool Reads = !I.Push || I.Kind == Operand::Memory;
        const auto Source = readAddress(S);
        const bool ReadFault = Reads && (Missing || !AtWrite) &&
                               Source < Suffix + Page &&
                               Source + I.Width > Suffix;
        const auto FaultStart = ReadFault ? Source : writeAddress(S);
        const auto FirstMissing = std::max(FaultStart, Suffix);
        EXPECT_EQ(Exit.Fault->Address, FirstMissing);
        EXPECT_EQ(Exit.Fault->Size, FaultStart + I.Width - FirstMissing);
        EXPECT_EQ(Exit.Fault->Access, ReadFault ? BackendAccessKind::Read
                                                : BackendAccessKind::Write);
        EXPECT_EQ(snapshot(), S);
        EXPECT_EQ(memory(), M);
        EXPECT_TRUE(CPU->takeRecoverableFault());
        EXPECT_FALSE(CPU->takeRecoverableFault());
        if (Missing)
          llvm::cantFail(CPU->mapAlias(
              Suffix, Alias, Page, Read | Write | Execute | UserAccessible));
        else
          llvm::cantFail(CPU->protect(Suffix, Page,
                                      Read | Write | Execute | UserAccessible));
        Pages.pop_back();
        Pages.push_back(Suffix);
        auto After = S;
        auto Contents = memory();
        expected(After, Contents);
        const auto Retried = run();
        ASSERT_EQ(Retried.Kind, ExecutionExitKind::Stopped)
            << Retried.Diagnostic;
        EXPECT_EQ(snapshot(), After);
        EXPECT_EQ(memory(), Contents);
      }
  }
}
TEST_P(X64Stack, DeviceOperandsRejectBeforeObserversOrCallbacks) {
  const auto &I = instruction();
  for (bool AtWrite : {false, true}) {
    if (AtWrite ? (!I.Push && I.Kind != Operand::Memory)
                : (I.Push && I.Kind != Operand::Memory))
      continue;
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    if (I.Location == Address::IP) {
      Entry = Code + Page - IPOffset / 2;
      llvm::cantFail(CPU->setReg(X64Register::PC, Entry));
      llvm::cantFail(CPU->write(Entry, I.Bytes));
    }
    const auto S = snapshot();
    const auto A = (AtWrite ? writeAddress(S) : readAddress(S)) & ~(Page - 1);
    unsigned Callbacks = 0, Observations = 0;
    GuestMMIOCallbacks IO;
    IO.Validate = [&](uint64_t, unsigned, bool) {
      ++Callbacks;
      return llvm::Error::success();
    };
    IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Callbacks;
      return Seed;
    };
    IO.Write = [&](uint64_t, unsigned, uint64_t) {
      ++Callbacks;
      return llvm::Error::success();
    };
    llvm::cantFail(CPU->addressSpace()->unmap(A, Page));
    Pages.erase(std::find(Pages.begin(), Pages.end(), A));
    const auto M = memory();
    auto Mapping = CPU->mapMMIO(A, Page, std::move(IO));
    if (GetParam().B.Contract == ExecutionContract::CheckedUserX64) {
      EXPECT_TRUE(bool(Mapping));
      llvm::consumeError(std::move(Mapping));
    } else {
      ASSERT_FALSE(bool(Mapping)) << llvm::toString(std::move(Mapping));
      BackendHooks H;
      H.Read = [&](uint64_t, unsigned) { ++Observations; };
      H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
      const auto Exit = run(std::move(H));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
    }
    EXPECT_EQ(Callbacks, 0u);
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(snapshot(), S);
    EXPECT_EQ(memory(), M);
  }
}
TEST_P(X64Stack, LockedFormsRejectWithoutMemoryObservations) {
  std::vector<uint8_t> Bytes{Lock};
  Bytes.insert(Bytes.end(), instruction().Bytes.begin(),
               instruction().Bytes.end());
  llvm::cantFail(CPU->write(Entry, Bytes));
  const auto S = snapshot();
  const auto M = memory();
  unsigned Observations = 0;
  BackendHooks H;
  H.Read = [&](uint64_t, unsigned) { ++Observations; };
  H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
      << Exit.Diagnostic;
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(snapshot(), S);
  EXPECT_EQ(memory(), M);
}
TEST_P(X64Stack, SavedContextResumesAgainstCommittedRAM) {
  const auto Before = snapshot();
  auto Saved = llvm::cantFail(CPU->saveContext());
  for (bool Restore : {false, true}) {
    if (Restore)
      llvm::cantFail(CPU->restoreContext(*Saved));
    EXPECT_EQ(snapshot(), Before);
    auto S = snapshot();
    auto M = memory();
    expected(S, M);
    const auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), S);
    EXPECT_EQ(memory(), M);
  }
}
TEST(X64StackOracle, OriginalHostInstructionsMatchWidthsAndAddresses) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned Executed = 0, GuestOnly = 0;
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    if (I.Location == Address::FS || I.Location == Address::GS) {
      ++GuestOnly;
      continue;
    }
    std::error_code EC;
    constexpr unsigned RW =
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE;
    auto CodeBlock =
        llvm::sys::Memory::allocateMappedMemory(2 * Page, nullptr, RW, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto ReleaseCode = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(CodeBlock); });
    llvm::sys::MemoryBlock Hint(reinterpret_cast<void *>(Data - 2 * Page),
                                2 * Page);
    auto DataBlock =
        llvm::sys::Memory::allocateMappedMemory(2 * Page, &Hint, RW, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto ReleaseData = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(DataBlock); });
    Hint = llvm::sys::MemoryBlock(reinterpret_cast<void *>(Stack - 2 * Page),
                                  2 * Page);
    auto StackBlock =
        llvm::sys::Memory::allocateMappedMemory(2 * Page, &Hint, RW, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto ReleaseStack = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(StackBlock); });
    const uint64_t DB = reinterpret_cast<uintptr_t>(DataBlock.base());
    const uint64_t SB = reinterpret_cast<uintptr_t>(StackBlock.base());
    if ((I.Location == Address::Address32 && DB + 2 * Page > UINT32_MAX) ||
        (I.Location == Address::StackAddress32 && SB + 2 * Page > UINT32_MAX)) {
      ++GuestOnly;
      continue;
    }
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
    const uint64_t Start =
        (Page - IPOffset + OracleAlignment - Program.size()) &
        ~(OracleAlignment - 1);
    const uint64_t Entry =
        reinterpret_cast<uintptr_t>(CodeBlock.base()) + Start + Program.size();
    Append(I.Bytes);
    Append(OracleSave);
    ASSERT_LT(Start + Program.size(), Page);
    ASSERT_GE(Entry + I.Bytes.size() + IPOffset,
              reinterpret_cast<uintptr_t>(CodeBlock.base()) + Page);
    std::memset(CodeBlock.base(), Nop, 2 * Page);
    auto *CodeStart = static_cast<uint8_t *>(CodeBlock.base()) + Start;
    std::memcpy(CodeStart, Program.data(), Program.size());
    // The original RIP-relative store reaches the second, writable page.
    // Executable instructions and their continuation stay in the first page.
    EC = llvm::sys::Memory::protectMappedMemory(
        llvm::sys::MemoryBlock(CodeBlock.base(), Page),
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(CodeStart, Program.size());
    auto Execute = reinterpret_cast<void (*)(uint64_t *)>(CodeStart);
    for (uint64_t Input : {uint64_t(0), Seed, UINT64_MAX}) {
      std::memset(DataBlock.base(), Fill, 2 * Page);
      std::memset(StackBlock.base(), Fill, 2 * Page);
      State S{{CPURegister::X64AX, {Input, 0}},
              {CPURegister::X64BX, {DB + Offset, 0}},
              {CPURegister::X64CX, {Index, 0}},
              {CPURegister::X64R12, {DB + Offset, 0}},
              {CPURegister::X64SP, {SB + Offset, 0}},
              {CPURegister::X64FLAGS, {Flags, 0}},
              {CPURegister::X64PC, {Entry, 0}}};
      Expectation Reference{I, Entry};
      if (!I.Push || I.Kind == Operand::Memory) {
        auto *Source = reinterpret_cast<uint8_t *>(Reference.readAddress(S));
        for (unsigned N = 0; N < I.Width; ++N)
          Source[N] = uint8_t(Input >> (N * ByteBits));
      }
      auto ReadMemory = [&] {
        RAM M;
        for (auto Base : {reinterpret_cast<uintptr_t>(CodeBlock.base()),
                          uintptr_t(DB), uintptr_t(SB)})
          for (unsigned N = 0; N < 2; ++N) {
            const auto A = Base + N * Page;
            const auto *Bytes = reinterpret_cast<const uint8_t *>(A);
            M[A] = std::vector<uint8_t>(Bytes, Bytes + Page);
          }
        return M;
      };
      auto M = ReadMemory();
      std::array<uint64_t, OracleWords> Packet{};
#define NEVERD_STACK_ORACLE_REGISTER(Name, N)                                  \
  Packet[N] = S[CPURegister::X64##Name][0];
#include "X64StackCases.def"
#undef NEVERD_STACK_ORACLE_REGISTER
      Reference.expected(S, M);
      Execute(Packet.data());
      ++Executed;
#define NEVERD_STACK_ORACLE_REGISTER(Name, N)                                  \
  EXPECT_EQ(Packet[N + OracleFields], S[CPURegister::X64##Name][0]);
#include "X64StackCases.def"
#undef NEVERD_STACK_ORACLE_REGISTER
      EXPECT_EQ(ReadMemory(), M);
    }
  }
  EXPECT_GT(Executed, 0u);
  std::cout << OracleExecuted << Executed << OracleGuestOnly << GuestOnly
            << '\n';
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Stack,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation

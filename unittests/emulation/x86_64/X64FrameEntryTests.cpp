//===- X64FrameEntryTests.cpp - Ordered frame entry and fault state ---===//
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

#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_FRAME_ENTRY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_FRAME_ENTRY_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_FRAME_ENTRY_ORACLE_BYTES(Name, ...)                             \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_ORACLE_BYTES
#undef NEVERD_FRAME_ENTRY_TEXT
#undef NEVERD_FRAME_ENTRY_VALUE
struct Instruction {
  const char *Name;
  unsigned Width;
  std::vector<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_FRAME_ENTRY_CASE(Name, Width, ...) {#Name, Width, {__VA_ARGS__}},
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_CASE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_FRAME_ENTRY_BACKEND(Name, Kind, Contract)                       \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_BACKEND
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
class X64FrameEntry : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint64_t> Pages;
  std::vector<uint8_t> Bytes;
  std::map<uint64_t, uint64_t> Aliases;
  const Instruction &instruction() const { return GetParam().I; }
  bool userMode() const {
    return GetParam().B.Contract == ExecutionContract::CheckedUserX64;
  }
  void SetUp() override { initialize(); }
  void initialize(uint64_t SP = Data + StackPage * Page + Offset,
                  uint64_t BP = Data + FramePage * Page + Offset,
                  unsigned Allocate = Allocation,
                  unsigned Level = DefaultNesting) {
    Pages.clear();
    Aliases.clear();
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
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->write(Code, std::vector<uint8_t>(Page, Nop)));
    for (unsigned N = 0; N < DataPages; ++N) {
      const auto A = Data + N * Page;
      llvm::cantFail(CPU->map(A, Page, Read | Write | UserAccessible));
      std::vector<uint8_t> Contents(Page);
      for (unsigned J = 0; J < Page; ++J)
        Contents[J] = uint8_t((N * Page + J) * PatternMultiplier + Fill);
      llvm::cantFail(CPU->write(A, Contents));
      llvm::cantFail(
          CPU->mapAlias(Mirror + N * Page, A, Page, Read | UserAccessible));
      Pages.push_back(A);
    }
    Bytes = instruction().Bytes;
    Bytes[Bytes.size() - ImmediateBytes] = uint8_t(Allocate);
    Bytes[Bytes.size() - ImmediateBytes + 1] = uint8_t(Allocate >> ByteBits);
    Bytes.back() = uint8_t(Level);
    llvm::cantFail(CPU->write(Code, Bytes));
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), Seed + R));
    llvm::cantFail(CPU->setReg(X64Register::SP, SP));
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
      llvm::cantFail(CPU->setXmm(N, {Seed + N, ~Seed - N}));
    for (unsigned N = 0; N < FPCount; ++N)
      llvm::cantFail(CPU->writeRegister(
          CPURegister(unsigned(CPURegister::X64FP0) + N), {Seed + N, FPHigh}));
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
        llvm::cantFail(CPU->addressSpace()->read(Mirror + A - Data, Bytes));
      else
        llvm::cantFail(CPU->snapshotBacking(Mirror + A - Data, Bytes));
    }
    return Result;
  }
  uint8_t &byte(RAM &R, uint64_t A) const {
    auto P = A & ~(Page - 1);
    if (auto I = Aliases.find(P); I != Aliases.end())
      P = I->second;
    return R.at(P).at(A % Page);
  }
  struct Observation {
    unsigned Permission;
    uint64_t Address, Value;
  };
  std::vector<Observation> expected(State &S, RAM &R, unsigned Stores,
                                    bool Retires = true) const {
    const unsigned Width = instruction().Width;
    const unsigned Level = Bytes.back() & NestingMask;
    const uint64_t SP = S.at(CPURegister::X64SP)[0];
    const uint64_t BP = S.at(CPURegister::X64BP)[0];
    const uint64_t Mask = UINT64_MAX >> ((WordBytes - Width) * ByteBits);
    std::vector<Observation> Result;
    for (unsigned I = 0; I < Stores; ++I) {
      uint64_t Value = I ? SP - Width : BP;
      if (I && I != Level) {
        const uint64_t A = BP - I * Width;
        Value = 0;
        for (unsigned J = 0; J < Width; ++J)
          Value |= uint64_t(byte(R, A + J)) << (J * ByteBits);
        Result.push_back({Read, A, 0});
      }
      const uint64_t A = SP - (I + 1) * Width;
      for (unsigned J = 0; J < Width; ++J)
        byte(R, A + J) = uint8_t(Value >> (J * ByteBits));
      Result.push_back({Write, A, Value & Mask});
    }
    if (Retires) {
      const unsigned Allocate =
          Bytes[Bytes.size() - ImmediateBytes] |
          unsigned(Bytes[Bytes.size() - ImmediateBytes + 1]) << ByteBits;
      S[CPURegister::X64BP][0] = (BP & ~Mask) | ((SP - Width) & Mask);
      S[CPURegister::X64SP][0] = SP - Stores * Width - Allocate;
      S[CPURegister::X64PC][0] += Bytes.size();
    }
    return Result;
  }
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t Size) {
      if (PC == Code)
        EXPECT_EQ(Size, Bytes.size());
      else {
        EXPECT_EQ(PC, Code + Bytes.size());
        CPU->stop();
      }
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};
TEST_P(X64FrameEntry, WidthsPrefixesNestingAndOverlappingFrames) {
  const unsigned Levels[] = {
#define NEVERD_FRAME_ENTRY_LEVEL(Value) Value,
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_LEVEL
  };
  for (unsigned Level : Levels)
    for (int Delta : {-1, 0, 1}) {
      const auto SP = Data + StackPage * Page + Offset;
      initialize(SP, SP + Delta * int(instruction().Width), Allocation, Level);
      if (HasFatalFailure() || IsSkipped())
        return;
      const auto Before = snapshot();
      const auto Original = memory();
      auto After = Before;
      auto ExpectedRAM = Original;
      const auto Events =
          expected(After, ExpectedRAM, 1 + (Level & NestingMask));
      size_t Observed = 0;
      auto Observe = [&](unsigned P, uint64_t A, unsigned Size, uint64_t V) {
        ASSERT_LT(Observed, Events.size());
        const auto &E = Events[Observed++];
        EXPECT_EQ(P, E.Permission);
        EXPECT_EQ(A, E.Address);
        EXPECT_EQ(Size, instruction().Width);
        if (P == Write)
          EXPECT_EQ(V, E.Value);
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(true), Original);
      };
      BackendHooks H;
      H.Read = [&](uint64_t A, unsigned N) { Observe(Read, A, N, 0); };
      H.Write = [&](uint64_t A, unsigned N, uint64_t V) {
        Observe(Write, A, N, V);
      };
      const auto Exit = run(std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(Observed, Events.size());
      EXPECT_EQ(snapshot(), After);
      EXPECT_EQ(memory(), ExpectedRAM);
    }
}

TEST_P(X64FrameEntry,
       OrderedFaultsKeepCompletedStoresAndRestartFromCurrentRAM) {
  const unsigned Width = instruction().Width;
  struct Scenario {
    const char *Name;
    uint64_t SP, BP, Guard;
    uint64_t Allocate;
    unsigned Level, Stores, Permission;
    uint64_t Fault;
    unsigned Size;
  };
  const Scenario Scenarios[] = {
#define NEVERD_FRAME_ENTRY_FAULT(Name, ...) {#Name, __VA_ARGS__},
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_FAULT
  };
  for (const auto &S : Scenarios)
    for (bool Missing : {false, true}) {
      SCOPED_TRACE(S.Name);
      initialize(Data + S.SP, Data + S.BP, S.Allocate, S.Level);
      if (HasFatalFailure() || IsSkipped())
        return;
      const auto Before = snapshot();
      auto Partial = memory();
      auto Unchanged = Before;
      const auto Events = expected(Unchanged, Partial, S.Stores, false);
      if (Missing)
        llvm::cantFail(CPU->addressSpace()->unmap(Data + S.Guard * Page, Page));
      else
        llvm::cantFail(CPU->protect(Data + S.Guard * Page, Page,
                                    (S.Permission == Read ? Write : Read) |
                                        UserAccessible));
      unsigned Reads = 0, Writes = 0, Faults = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, unsigned) { ++Reads; };
      H.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      H.RecoverableFault = [&](const BackendFault &F) {
        ++Faults;
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(true), Partial);
        EXPECT_EQ(F.Address, Data + S.Fault);
        EXPECT_EQ(F.Size, S.Size);
        EXPECT_EQ(F.Access, S.Permission == Write ? BackendAccessKind::Write
                                                  : BackendAccessKind::Read);
        EXPECT_EQ(F.Kind, Missing ? BackendFaultKind::UnmappedMemory
                                  : BackendFaultKind::Protection);
        return true;
      };
      auto Exit = run(std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      EXPECT_EQ(Faults, 1u);
      EXPECT_EQ(Reads + Writes, Events.size());
      EXPECT_EQ(Writes, S.Stores);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), Partial);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      if (Missing)
        llvm::cantFail(CPU->mapAlias(Data + S.Guard * Page,
                                     Mirror + S.Guard * Page, Page,
                                     Read | Write | UserAccessible));
      else
        llvm::cantFail(CPU->protect(Data + S.Guard * Page, Page,
                                    Read | Write | UserAccessible));
      auto After = Before;
      expected(After, Partial, 1 + (S.Level & NestingMask));
      Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(snapshot(), After);
      EXPECT_EQ(memory(), Partial);
    }
}

TEST_P(X64FrameEntry, VirtualAliasesReadTheEarlierPhysicalStores) {
  for (unsigned N = 0; N < DataPages; ++N) {
    llvm::cantFail(CPU->mapAlias(Alias + N * Page, Data + N * Page, Page,
                                 Read | UserAccessible));
    Aliases[Alias + N * Page] = Data + N * Page;
  }
  llvm::cantFail(
      CPU->setReg(X64Register::BP, Alias + StackPage * Page + Offset));
  auto After = snapshot();
  auto ExpectedRAM = memory();
  expected(After, ExpectedRAM, 1 + DefaultNesting);
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), After);
  EXPECT_EQ(memory(), ExpectedRAM);
  std::vector<uint8_t> AliasRAM(Page);
  llvm::cantFail(CPU->read(Alias + StackPage * Page, AliasRAM));
  EXPECT_EQ(AliasRAM, ExpectedRAM.at(Data + StackPage * Page));
}

TEST_P(X64FrameEntry, FinalProbeAcceptsWriteOnlyRAMWithoutDataObservations) {
  const unsigned Width = instruction().Width;
  initialize(Data + StackPage * Page + Offset, Data + FramePage * Page + Offset,
             (StackPage - ProbePage) * Page - (DefaultNesting + 1) * Width);
  if (HasFatalFailure() || IsSkipped())
    return;
  auto After = snapshot();
  auto ExpectedRAM = memory();
  expected(After, ExpectedRAM, DefaultNesting + 1);
  llvm::cantFail(
      CPU->protect(Data + ProbePage * Page, Page, Write | UserAccessible));
  unsigned Reads = 0, Writes = 0;
  BackendHooks H;
  H.Read = [&](uint64_t A, unsigned) {
    ++Reads;
    EXPECT_NE(A / Page, (Data + ProbePage * Page) / Page);
  };
  H.Write = [&](uint64_t A, unsigned, uint64_t) {
    ++Writes;
    EXPECT_NE(A / Page, (Data + ProbePage * Page) / Page);
  };
  auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Reads, DefaultNesting - 1);
  EXPECT_EQ(Writes, DefaultNesting + 1);
  EXPECT_EQ(snapshot(), After);
  EXPECT_EQ(memory(), ExpectedRAM);
}

TEST_P(X64FrameEntry, ObserverStopsAndFailuresDiscardTheWholeInstruction) {
  for (bool Throw : {false, true})
    for (unsigned At : {1u, 2u, 5u}) {
      initialize();
      if (HasFatalFailure() || IsSkipped())
        return;
      const auto Before = snapshot();
      const auto Original = memory();
      unsigned Seen = 0;
      auto Observe = [&] {
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(true), Original);
        if (++Seen == At) {
          if (Throw)
            throw std::runtime_error(ObserverFailure);
          CPU->stop();
        }
      };
      BackendHooks H;
      H.Read = [&](uint64_t, unsigned) { Observe(); };
      H.Write = [&](uint64_t, unsigned, uint64_t) { Observe(); };
      auto Exit = run(std::move(H));
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Seen, At);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), Original);
    }
}

TEST_P(X64FrameEntry, PermissionFaultCancellationPublishesNoPrefix) {
  const auto SP = Data + StackPage * Page + Offset;
  initialize(SP, Data + 2 * Page + instruction().Width);
  if (HasFatalFailure() || IsSkipped())
    return;
  llvm::cantFail(CPU->protect(Data + 2 * Page, Page, Write | UserAccessible));
  const auto Before = snapshot();
  const auto Original = memory();
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Writes;
    CPU->stop();
  };
  H.RecoverableFault = [](const BackendFault &) {
    ADD_FAILURE();
    return true;
  };
  const auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), Original);
}

TEST_P(X64FrameEntry, PrivilegeDenialPreservesOnlyTheCompletedStores) {
  initialize(Data + StackPage * Page + Offset,
             Data + 2 * Page + instruction().Width);
  if (HasFatalFailure() || IsSkipped())
    return;
  auto After = snapshot();
  auto ExpectedRAM = memory();
  expected(After, ExpectedRAM, userMode() ? 1 : 1 + DefaultNesting,
           !userMode());
  llvm::cantFail(CPU->protect(Data + 2 * Page, Page, Read | Write));
  const auto Exit = run();
  EXPECT_EQ(Exit.Kind, userMode() ? ExecutionExitKind::GuestFault
                                  : ExecutionExitKind::Stopped)
      << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), After);
  EXPECT_EQ(memory(), ExpectedRAM);
}

TEST_P(X64FrameEntry, NoncanonicalAncestorRetainsTheFirstStore) {
  for (auto BP :
       {Noncanonical + instruction().Width, Noncanonical + 1, uint64_t(0)}) {
    initialize(Data + StackPage * Page + Offset, BP);
    if (HasFatalFailure() || IsSkipped())
      return;
    auto Before = snapshot();
    auto Partial = memory();
    expected(Before, Partial, 1, false);
    BackendHooks H;
    H.RecoverableFault = [](const BackendFault &) { return true; };
    const auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Partial);
  }
}

TEST_P(X64FrameEntry, ImmediateBytesCannotBeMistakenForRejectedPrefixes) {
  const unsigned Immediates[] = {
#define NEVERD_FRAME_ENTRY_REJECTED_PREFIX(Value) Value,
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_REJECTED_PREFIX
  };
  for (auto Value : Immediates) {
    initialize(Data + StackPage * Page + Offset,
               Data + FramePage * Page + Offset, Value, Value);
    if (HasFatalFailure() || IsSkipped())
      return;
    auto After = snapshot();
    auto ExpectedRAM = memory();
    expected(After, ExpectedRAM, 1 + (Value & NestingMask));
    auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), After);
    EXPECT_EQ(memory(), ExpectedRAM);
  }
}

TEST_P(X64FrameEntry, RejectedPrefixesPublishNoObservationsOrEffects) {
  const std::vector<uint8_t> Prefixes[] = {
#define NEVERD_FRAME_ENTRY_REJECTED_PREFIX(Value) {Value},
#define NEVERD_FRAME_ENTRY_UNSUPPORTED_PREFIX(Name, ...) {__VA_ARGS__},
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_REJECTED_PREFIX
#undef NEVERD_FRAME_ENTRY_UNSUPPORTED_PREFIX
  };
  for (auto Prefix : Prefixes) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    auto Bytes = instruction().Bytes;
    Bytes.insert(Bytes.begin(), Prefix.begin(), Prefix.end());
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
TEST_P(X64FrameEntry, DeviceFramesRejectBeforeAnyObservationOrStore) {
  for (unsigned Guard :
       {unsigned(StackPage), unsigned(FramePage), unsigned(ProbePage)}) {
    const unsigned Width = instruction().Width;
    initialize(Data + StackPage * Page + Offset,
               Data + FramePage * Page + Offset,
               (StackPage - ProbePage) * Page - (DefaultNesting + 1) * Width);
    if (HasFatalFailure() || IsSkipped())
      return;
    unsigned Calls = 0, Observations = 0;
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
    llvm::cantFail(CPU->addressSpace()->unmap(Data + Guard * Page, Page));
    auto Mapping = CPU->mapMMIO(Data + Guard * Page, Page, std::move(Device));
    if (userMode()) {
      EXPECT_TRUE(bool(Mapping));
      llvm::consumeError(std::move(Mapping));
      EXPECT_EQ(Calls, 0u);
      return;
    }
    ASSERT_FALSE(bool(Mapping)) << llvm::toString(std::move(Mapping));
    const auto Before = snapshot();
    const auto Original = memory();
    BackendHooks H;
    H.Read = [&](uint64_t, unsigned) { ++Observations; };
    H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), Original);
    EXPECT_EQ(Calls, 0u);
    EXPECT_EQ(Observations, 0u);
  }
}
TEST_P(X64FrameEntry, ContextRestoreRetainsPartialRAMAndUsesNewInput) {
  const unsigned Width = instruction().Width;
  const auto SP = Data + StackPage * Page + Offset;
  initialize(SP, SP,
             (StackPage - ProbePage) * Page - (DefaultNesting + 1) * Width);
  if (HasFatalFailure() || IsSkipped())
    return;
  auto Before = snapshot();
  auto Saved = llvm::cantFail(CPU->saveContext());
  auto ExpectedRAM = memory();
  expected(Before, ExpectedRAM, DefaultNesting + 1, false);
  llvm::cantFail(
      CPU->protect(Data + ProbePage * Page, Page, Read | UserAccessible));
  BackendHooks H;
  H.RecoverableFault = [](const BackendFault &) { return true; };
  auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  ASSERT_TRUE(CPU->takeRecoverableFault());
  llvm::cantFail(CPU->restoreContext(*Saved));
  EXPECT_EQ(memory(), ExpectedRAM);
  llvm::cantFail(CPU->protect(Data + ProbePage * Page, Page,
                              Read | Write | UserAccessible));
  llvm::cantFail(CPU->writeInteger(SP - 2 * Width, Seed, Width));
  ExpectedRAM = memory();
  expected(Before, ExpectedRAM, DefaultNesting + 1);
  Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), ExpectedRAM);
}

#if defined(__linux__) && defined(__x86_64__)
struct HostFault {
  uint64_t Signal, PC, BP, SP, Flags, Address;
};
int HostFaultPipe = -1;
void captureHostFault(int Signal, siginfo_t *Info, void *Context) {
  const auto &G = static_cast<ucontext_t *>(Context)->uc_mcontext.gregs;
  const HostFault F{
      uint64_t(Signal),     uint64_t(G[REG_RIP]),
      uint64_t(G[REG_RBP]), uint64_t(G[REG_RSP]),
      uint64_t(G[REG_EFL]), reinterpret_cast<uintptr_t>(Info->si_addr)};
  const auto Written = ::write(HostFaultPipe, &F, sizeof(F));
  _exit(Written == sizeof(F) ? 0 : 1);
}
#endif
TEST(X64FrameEntryOracle,
     OriginalHostFaultsRetainOrderedStoresAndEntryRegisters) {
#if defined(__linux__) && defined(__x86_64__)
  auto *DataBytes = static_cast<uint8_t *>(
      mmap(nullptr, DataPages * Page, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(DataBytes, MAP_FAILED);
  auto ReleaseData =
      llvm::scope_exit([&] { munmap(DataBytes, DataPages * Page); });
  void *Alternate =
      mmap(nullptr, OracleDataPages * Page, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(Alternate, MAP_FAILED);
  auto ReleaseStack =
      llvm::scope_exit([&] { munmap(Alternate, OracleDataPages * Page); });
  const uint64_t Base = reinterpret_cast<uintptr_t>(DataBytes);
  unsigned Executed = 0;
  for (const auto &I : Instructions) {
    const unsigned Width = I.Width;
    struct Scenario {
      const char *Name;
      uint64_t SP, BP, Guard, Allocate;
      unsigned Level, Stores, Permission;
      uint64_t Fault;
      unsigned Size;
    };
    const Scenario Scenarios[] = {
#define NEVERD_FRAME_ENTRY_FAULT(Name, ...) {#Name, __VA_ARGS__},
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_FAULT
    };
    for (const auto &S : Scenarios) {
      SCOPED_TRACE(I.Name);
      SCOPED_TRACE(S.Name);
      std::vector<uint8_t> ExpectedRAM(DataPages * Page);
      for (size_t N = 0; N < ExpectedRAM.size(); ++N)
        ExpectedRAM[N] = uint8_t(N * PatternMultiplier + Fill);
      std::memcpy(DataBytes, ExpectedRAM.data(), ExpectedRAM.size());
      const uint64_t SP = Base + S.SP, BP = Base + S.BP;
      for (unsigned N = 0; N < S.Stores; ++N) {
        uint64_t Value = N ? SP - Width : BP;
        if (N && N != S.Level) {
          Value = 0;
          for (unsigned J = 0; J < Width; ++J)
            Value |= uint64_t(ExpectedRAM.at(S.BP - N * Width + J))
                     << (J * ByteBits);
        }
        for (unsigned J = 0; J < Width; ++J)
          ExpectedRAM.at(S.SP - (N + 1) * Width + J) =
              uint8_t(Value >> (J * ByteBits));
      }
      std::vector<uint8_t> Program;
      auto Append = [&](llvm::ArrayRef<uint8_t> B) {
        Program.insert(Program.end(), B.begin(), B.end());
      };
      Append(OraclePrefix);
      Append(SysVArgument);
      Append(OracleLoad);
      const size_t FaultOffset = Program.size();
      auto Bytes = I.Bytes;
      Bytes[Bytes.size() - ImmediateBytes] = uint8_t(S.Allocate);
      Bytes[Bytes.size() - ImmediateBytes + 1] =
          uint8_t(S.Allocate >> ByteBits);
      Bytes.back() = uint8_t(S.Level);
      Append(Bytes);
      Append(OracleSave);
      std::error_code EC;
      auto Block = llvm::sys::Memory::allocateMappedMemory(
          Page, nullptr,
          llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      auto Release = llvm::scope_exit(
          [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
      std::memcpy(Block.base(), Program.data(), Program.size());
      EC = llvm::sys::Memory::protectMappedMemory(
          Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      llvm::sys::Memory::InvalidateInstructionCache(Block.base(),
                                                    Program.size());
      int Pipe[2];
      ASSERT_EQ(pipe(Pipe), 0);
      const pid_t Child = fork();
      ASSERT_GE(Child, 0);
      if (!Child) {
        close(Pipe[0]);
        HostFaultPipe = Pipe[1];
        stack_t Stack{};
        Stack.ss_sp = Alternate;
        Stack.ss_size = OracleDataPages * Page;
        struct sigaction Action{};
        Action.sa_sigaction = captureHostFault;
        Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&Action.sa_mask);
        if (sigaltstack(&Stack, nullptr) ||
            sigaction(SIGSEGV, &Action, nullptr) ||
            sigaction(SIGBUS, &Action, nullptr) ||
            mprotect(DataBytes + S.Guard * Page, Page,
                     S.Permission == Write ? PROT_READ : PROT_NONE))
          _exit(1);
        std::array<uint64_t, OracleWords> Packet{BP, SP, Flags};
        reinterpret_cast<void (*)(uint64_t *)>(Block.base())(Packet.data());
        _exit(1);
      }
      close(Pipe[1]);
      HostFault F{};
      const auto Received = ::read(Pipe[0], &F, sizeof(F));
      close(Pipe[0]);
      int Status = 0;
      ASSERT_EQ(waitpid(Child, &Status, 0), Child);
      ASSERT_TRUE(WIFEXITED(Status));
      ASSERT_EQ(WEXITSTATUS(Status), 0);
      ASSERT_EQ(Received, sizeof(F));
      EXPECT_EQ(F.Signal, SIGSEGV);
      EXPECT_EQ(F.PC, reinterpret_cast<uintptr_t>(Block.base()) + FaultOffset);
      EXPECT_EQ(F.BP, BP);
      EXPECT_EQ(F.SP, SP);
      EXPECT_EQ(F.Flags & ~FaultResumeFlag, Flags);
      EXPECT_EQ(F.Address, Base + S.Fault);
      EXPECT_EQ(std::memcmp(DataBytes, ExpectedRAM.data(), ExpectedRAM.size()),
                0);
      ++Executed;
    }
  }
  std::cout << FaultOracleExecuted << Executed << '\n';
#else
  GTEST_SKIP() << FaultOracleUnavailable;
#endif
}

TEST(X64FrameEntryOracle, OriginalHostInstructionsDetermineCopiesAndWidths) {
#if defined(__x86_64__) || defined(_M_X64)
  const unsigned Levels[] = {
#define NEVERD_FRAME_ENTRY_LEVEL(Value) Value,
#include "X64FrameEntryCases.def"
#undef NEVERD_FRAME_ENTRY_LEVEL
  };
  std::error_code EC;
  auto DataBlock = llvm::sys::Memory::allocateMappedMemory(
      OracleDataPages * Page, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  auto ReleaseData = llvm::scope_exit(
      [&] { (void)llvm::sys::Memory::releaseMappedMemory(DataBlock); });
  const uint64_t Base = reinterpret_cast<uintptr_t>(DataBlock.base());
  auto *Actual = static_cast<uint8_t *>(DataBlock.base());
  std::vector<uint8_t> ExpectedRAM(OracleDataPages * Page);
  unsigned Executed = 0;
  for (const auto &I : Instructions)
    for (unsigned Level : Levels)
      for (unsigned Allocate :
           {0u, unsigned(Allocation), unsigned(UINT16_MAX)}) {
        SCOPED_TRACE(I.Name);
        std::vector<uint8_t> Program;
        auto Append = [&](llvm::ArrayRef<uint8_t> B) {
          Program.insert(Program.end(), B.begin(), B.end());
        };
        Append(OraclePrefix);
#ifdef _WIN32
        Append(Win64Argument);
#else
        Append(SysVArgument);
#endif
        Append(OracleLoad);
        auto Bytes = I.Bytes;
        Bytes[Bytes.size() - ImmediateBytes] = uint8_t(Allocate);
        Bytes[Bytes.size() - ImmediateBytes + 1] =
            uint8_t(Allocate >> ByteBits);
        Bytes.back() = uint8_t(Level);
        Append(Bytes);
        Append(OracleSave);
        auto Block = llvm::sys::Memory::allocateMappedMemory(
            Page, nullptr,
            llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
        ASSERT_FALSE(bool(EC)) << EC.message();
        auto Release = llvm::scope_exit(
            [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
        std::memcpy(Block.base(), Program.data(), Program.size());
        EC = llvm::sys::Memory::protectMappedMemory(
            Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
        ASSERT_FALSE(bool(EC)) << EC.message();
        llvm::sys::Memory::InvalidateInstructionCache(Block.base(),
                                                      Program.size());
        auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
        for (int Delta : {-1, 0, 1}) {
          for (size_t N = 0; N < ExpectedRAM.size(); ++N)
            ExpectedRAM[N] = uint8_t(N * PatternMultiplier + Fill);
          std::memcpy(Actual, ExpectedRAM.data(), ExpectedRAM.size());
          const uint64_t SP = Base + OracleStackPage * Page + Offset;
          const uint64_t BP = SP + Delta * int(I.Width);
          const unsigned Nesting = Level & NestingMask;
          for (unsigned N = 0; N <= Nesting; ++N) {
            uint64_t Value = N ? SP - I.Width : BP;
            if (N && N != Nesting) {
              Value = 0;
              for (unsigned J = 0; J < I.Width; ++J)
                Value |= uint64_t(ExpectedRAM.at(BP - Base - N * I.Width + J))
                         << (J * ByteBits);
            }
            for (unsigned J = 0; J < I.Width; ++J)
              ExpectedRAM.at(SP - Base - (N + 1) * I.Width + J) =
                  uint8_t(Value >> (J * ByteBits));
          }
          std::array<uint64_t, OracleWords> Packet{BP, SP, Flags};
          Execute(Packet.data());
          ++Executed;
          const uint64_t Mask =
              UINT64_MAX >> ((WordBytes - I.Width) * ByteBits);
          EXPECT_EQ(Packet[OracleFields],
                    (BP & ~Mask) | ((SP - I.Width) & Mask));
          EXPECT_EQ(Packet[OracleFields + 1],
                    SP - (Nesting + 1) * I.Width - Allocate);
          EXPECT_EQ(Packet[OracleFields + 2], Flags);
          EXPECT_EQ(std::memcmp(Actual, ExpectedRAM.data(), ExpectedRAM.size()),
                    0);
        }
      }
  EXPECT_EQ(Executed, std::size(Instructions) * std::size(Levels) * 3 * 3);
  std::cout << OracleExecuted << Executed << '\n';
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64FrameEntry,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation

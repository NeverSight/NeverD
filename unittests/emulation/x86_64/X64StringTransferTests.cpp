//===- X64StringTransferTests.cpp - Restartable string load/store/move ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64StringTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace string_test;
#define NEVERD_STRING_CASE(Name, Size, Kind, ...)                              \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StringTransferCases.def"
#undef NEVERD_STRING_CASE

enum class Operation { Move, Store, Load };
struct StringCase {
  const char *Name;
  unsigned Size;
  Operation Kind;
  llvm::ArrayRef<uint8_t> Bytes;
  bool reads() const { return Kind != Operation::Store; }
  bool writes() const { return Kind != Operation::Load; }
};
const StringCase Cases[] = {
#define NEVERD_STRING_CASE(Name, Size, Kind, ...)                              \
  {#Name, Size, Operation::Kind, Name},
#include "X64StringTransferCases.def"
#undef NEVERD_STRING_CASE
};
class X64StringTransfer : public X64StringTest {};

TEST_P(X64StringTransfer, TransfersMatchHostAcrossWidthsCountsAndDirection) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (bool Repeat : {false, true})
      for (bool Backward : {false, true})
        for (uint64_t Count : {uint64_t(0), uint64_t(1), RepeatCount})
          for (uint64_t Destination : {DestinationOffset, SourceOffset + 1}) {
            SCOPED_TRACE(C.Name);
            SCOPED_TRACE(Repeat);
            SCOPED_TRACE(Backward);
            SCOPED_TRACE(Count);
            auto Expected = Before;
            const auto Bytes = encoding(C, Repeat);
            StringState Guest{InitialAX, Count, Data + SourceOffset,
                              Alias + Destination,
                              Flags | (Backward ? Direction : 0)};
            StringState Host{
                Guest.AX, Guest.CX,
                reinterpret_cast<uintptr_t>(Expected.data() + SourceOffset),
                reinterpret_cast<uintptr_t>(Expected.data() + Destination),
                Guest.Flags};
            const auto BeforeHost = Host;
            runHost(Bytes, Host);
            ASSERT_FALSE(HasFatalFailure());
            seed(Guest);
            unsigned Reads = 0, Writes = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t, unsigned Size) {
              EXPECT_EQ(Size, C.Size);
              ++Reads;
            };
            Hooks.Write = [&](uint64_t, unsigned Size, uint64_t) {
              EXPECT_EQ(Size, C.Size);
              ++Writes;
            };
            auto Exit = run(Bytes, std::move(Hooks));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
            expectState({Host.AX, Host.CX, Guest.SI + Host.SI - BeforeHost.SI,
                         Guest.DI + Host.DI - BeforeHost.DI, Host.Flags});
            EXPECT_EQ(memory(), Expected);
            EXPECT_EQ(memory(Alias), Expected);
            EXPECT_EQ(Reads, C.reads() ? (Repeat ? Count : 1) : 0);
            EXPECT_EQ(Writes, C.writes() ? (Repeat ? Count : 1) : 0);
            EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
          }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringTransfer, DirectionInstructionsPreserveOtherFlagsAndRegisters) {
  for (bool Backward : {false, true}) {
    StringState State{InitialAX, RepeatCount, Data + SourceOffset,
                      Alias + DestinationOffset,
                      Flags | (Backward ? Direction : 0)};
    seed(State);
    auto Exit = run(SetDirection);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    State.Flags = Flags | Direction;
    expectState(State);
    Exit = run(ClearDirection);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    State.Flags = Flags;
    expectState(State);
    EXPECT_EQ(memory(), Before);
  }
}

TEST_P(X64StringTransfer, ZeroCountAddressSizeMatchesHostWithoutMemory) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (bool Narrow : {false, true}) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Narrow);
      StringState State{
          InitialAX, 0,
          (Narrow && C.Kind == Operation::Move ? 0 : UpperBits) | SourceOffset,
          (Narrow && C.writes() ? 0 : UpperBits) | DestinationOffset, Flags};
      seed(State);
      const auto Bytes = encoding(C, true, Narrow);
      runHost(Bytes, State);
      ASSERT_FALSE(HasFatalFailure());
      unsigned Accesses = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
      auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      expectState(State);
      EXPECT_EQ(Accesses, 0u);
      EXPECT_EQ(memory(), Before);
    }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringTransfer, AmbiguousZeroCountUpperHalvesRejectBeforeEffects) {
  for (const auto &C : Cases)
    for (auto Register : {X64Register::CX, X64Register::SI, X64Register::DI}) {
      if ((Register == X64Register::SI && C.Kind != Operation::Move) ||
          (Register == X64Register::DI && !C.writes()))
        continue;
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(unsigned(Register));
      StringState State{InitialAX, 0, SourceOffset, DestinationOffset, Flags};
      (Register == X64Register::CX   ? State.CX
       : Register == X64Register::SI ? State.SI
                                     : State.DI) |= UpperBits;
      seed(State);
      unsigned Accesses = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
      EXPECT_EQ(run(encoding(C, true, true), std::move(Hooks)).Kind,
                ExecutionExitKind::UnsupportedOperation);
      EXPECT_EQ(Accesses, 0u);
      expectState(State);
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(reg(X64Register::PC), Code);
    }
}

TEST_P(X64StringTransfer, NarrowAddressesNormalizeOnlyParticipatingRegisters) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (bool Repeat : {false, true})
      for (bool Backward : {false, true}) {
        SCOPED_TRACE(C.Name);
        SCOPED_TRACE(Repeat);
        SCOPED_TRACE(Backward);
        StringState Guest{InitialAX, UpperBits | RepeatCount,
                          UpperBits | (Data + SourceOffset),
                          UpperBits | (Alias + DestinationOffset),
                          Flags | (Backward ? Direction : 0)};
        auto Expected = Before;
        StringState Host{
            Guest.AX, RepeatCount,
            reinterpret_cast<uintptr_t>(Expected.data() + SourceOffset),
            reinterpret_cast<uintptr_t>(Expected.data() + DestinationOffset),
            Guest.Flags};
        const auto BeforeHost = Host;
        runHost(encoding(C, Repeat), Host);
        ASSERT_FALSE(HasFatalFailure());
        seed(Guest);
        auto Exit = run(encoding(C, Repeat, true));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        expectState({Host.AX, Repeat ? Host.CX : Guest.CX,
                     C.reads() ? uint32_t(Guest.SI + Host.SI - BeforeHost.SI)
                               : Guest.SI,
                     C.writes() ? uint32_t(Guest.DI + Host.DI - BeforeHost.DI)
                                : Guest.DI,
                     Host.Flags});
        EXPECT_EQ(memory(), Expected);
      }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringTransfer,
       SourceSegmentsFollowAddressTruncationAndNeverBiasDestination) {
  llvm::cantFail(CPU->mapAlias(Data + SegmentBase, Data, PageSize,
                               Read | Write | UserAccessible));
  for (const auto &C : Cases)
    for (uint8_t Segment : {uint8_t(FSPrefix), uint8_t(GSPrefix)}) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Segment);
      StringState State{InitialAX, RepeatCount,
                        UpperBits | (Data + SourceOffset),
                        UpperBits | (Alias + DestinationOffset), Flags};
      seed(State);
      llvm::cantFail(
          CPU->writeRegister(CPURegister::X64FSBase, {SegmentBase, 0}));
      llvm::cantFail(CPU->setGSBase(SegmentBase));
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t A, unsigned Size) {
        EXPECT_EQ(A, SegmentBase + Data + SourceOffset + Reads * C.Size);
        EXPECT_EQ(Size, C.Size);
        ++Reads;
      };
      Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t) {
        EXPECT_EQ(A, Alias + DestinationOffset + Writes * C.Size);
        EXPECT_EQ(Size, C.Size);
        ++Writes;
      };
      auto Exit = run(encoding(C, true, true, Segment), std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(Reads, C.reads() ? RepeatCount : 0);
      EXPECT_EQ(Writes, C.writes() ? RepeatCount : 0);
      EXPECT_EQ(reg(X64Register::SI),
                C.reads() ? Data + SourceOffset + RepeatCount * C.Size
                          : State.SI);
      EXPECT_EQ(reg(X64Register::DI),
                C.writes() ? Alias + DestinationOffset + RepeatCount * C.Size
                           : State.DI);
      EXPECT_EQ(reg(X64Register::CX), 0u);
      EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    }
}

TEST_P(X64StringTransfer, NarrowRepeatedAddressesWrapAtEachElementBoundary) {
  llvm::cantFail(
      CPU->map(LastNarrowPage, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->map(0, PageSize, Read | Write | UserAccessible));
  for (const auto &C : Cases) {
    if (C.Size != 1)
      continue;
    for (bool Backward : {false, true}) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Backward);
      const uint64_t Start = Backward ? 0 : UINT32_MAX;
      StringState State{InitialAX, UpperBits | RepeatCount, UpperBits | Start,
                        UpperBits | Start, Flags | (Backward ? Direction : 0)};
      seed(State);
      for (unsigned I = 0; I < RepeatCount; ++I)
        llvm::cantFail(CPU->writeInteger(
            uint32_t(Start + (Backward ? -int64_t(I) : I)), Seed + I, 1));
      auto Exit = run(encoding(C, true, true));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      const auto End =
          uint32_t(Start + (Backward ? -int64_t(RepeatCount) : RepeatCount));
      EXPECT_EQ(reg(X64Register::SI), C.reads() ? End : State.SI);
      EXPECT_EQ(reg(X64Register::DI), C.writes() ? End : State.DI);
      EXPECT_EQ(reg(X64Register::AX), C.Kind == Operation::Load
                                          ? (InitialAX & ~uint64_t(UINT8_MAX)) |
                                                (Seed + RepeatCount - 1)
                                          : InitialAX);
      EXPECT_EQ(reg(X64Register::CX), 0u);
      EXPECT_EQ(reg(X64Register::FLAGS), State.Flags);
      for (unsigned I = 0; I < RepeatCount; ++I)
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(
                      uint32_t(Start + (Backward ? -int64_t(I) : I)), 1)),
                  C.Kind == Operation::Store ? uint8_t(InitialAX) : Seed + I);
    }
  }
}

TEST_P(X64StringTransfer, UnusedPointersAreNeverValidatedOrObserved) {
  for (const auto &C : Cases) {
    if (C.Kind == Operation::Move)
      continue;
    SCOPED_TRACE(C.Name);
    StringState State{InitialAX, RepeatCount,
                      C.reads() ? Data + SourceOffset : InvalidPointer,
                      C.writes() ? Data + DestinationOffset : InvalidPointer,
                      Flags};
    seed(State);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned) {
      EXPECT_NE(A, InvalidPointer);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t A, unsigned, uint64_t) {
      EXPECT_NE(A, InvalidPointer);
      ++Writes;
    };
    auto Exit = run(C.Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, unsigned(C.reads()));
    EXPECT_EQ(Writes, unsigned(C.writes()));
    EXPECT_EQ(reg(X64Register::CX), RepeatCount);
    EXPECT_EQ(reg(C.reads() ? X64Register::DI : X64Register::SI),
              InvalidPointer);
  }
}

TEST_P(X64StringTransfer, CrossPageElementsAndCancellationMatchHost) {
#if defined(__x86_64__) || defined(_M_X64)
  mapCrossingPages();
  for (const auto &C : Cases)
    for (unsigned Prefix = 1; Prefix < C.Size; ++Prefix)
      for (bool Stop : {false, true}) {
        SCOPED_TRACE(C.Name);
        SCOPED_TRACE(Prefix);
        SCOPED_TRACE(Stop);
        const uint64_t Crossing = Data + PageSize - Prefix;
        StringState State{
            InitialAX, RepeatCount, C.writes() ? Data + SourceOffset : Crossing,
            C.writes() ? Alias + PageSize - Prefix : InvalidPointer, Flags};
        seed(State);
        llvm::cantFail(CPU->writeInteger(Crossing, InitialAX, WordBytes));
        const auto Original = word(Crossing);
        auto Expected = Original;
        auto Source = Before;
        StringState Host{
            InitialAX, RepeatCount,
            reinterpret_cast<uintptr_t>(
                C.writes() ? Source.data() + SourceOffset : Expected.data()),
            reinterpret_cast<uintptr_t>(Expected.data()), Flags};
        runHost(C.Bytes, Host);
        ASSERT_FALSE(HasFatalFailure());
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, State.SI);
          EXPECT_EQ(Size, C.Size);
          EXPECT_EQ(word(Crossing), Original);
          expectState(State);
          ++Reads;
          if (Stop && !C.writes())
            CPU->stop();
        };
        Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
          EXPECT_EQ(A, State.DI);
          EXPECT_EQ(Size, C.Size);
          uint64_t ExpectedValue = 0;
          for (unsigned I = 0; I < C.Size; ++I)
            ExpectedValue |= uint64_t(Expected[I]) << (I * CHAR_BIT);
          EXPECT_EQ(Value, ExpectedValue);
          EXPECT_EQ(word(Crossing), Original);
          expectState(State);
          ++Writes;
          if (Stop)
            CPU->stop();
        };
        auto Exit = run(C.Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(word(Crossing), Stop ? Original : Expected);
        EXPECT_EQ(word(Alias + PageSize - Prefix), Stop ? Original : Expected);
        EXPECT_EQ(Reads, unsigned(C.reads()));
        EXPECT_EQ(Writes, unsigned(C.writes()));
        expectState(Stop ? State
                         : StringState{Host.AX, State.CX,
                                       State.SI + (C.reads() ? C.Size : 0),
                                       State.DI + (C.writes() ? C.Size : 0),
                                       Host.Flags});
        EXPECT_EQ(reg(X64Register::PC), Code + (Stop ? 0 : C.Bytes.size()));
      }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringTransfer, LaterPageFaultHasNoPartialElementAndCanResume) {
  enum class Fault { Unmapped, Permission, Supervisor };
  mapCrossingPages();
  for (const auto &C : Cases)
    for (unsigned Prefix = 1; Prefix < C.Size; ++Prefix)
      for (auto Failure :
           {Fault::Unmapped, Fault::Permission, Fault::Supervisor}) {
        if (Failure == Fault::Supervisor && !GetParam().User)
          continue;
        SCOPED_TRACE(C.Name);
        SCOPED_TRACE(Prefix);
        SCOPED_TRACE(int(Failure));
        const uint64_t Crossing = Data + PageSize - Prefix;
        StringState State{InitialAX, RepeatCount,
                          C.writes() ? Data + SourceOffset : Crossing,
                          C.writes() ? Crossing : InvalidPointer, Flags};
        seed(State);
        llvm::cantFail(CPU->writeInteger(Crossing, InitialAX, WordBytes));
        const auto Original = word(Crossing);
        if (Failure == Fault::Unmapped)
          llvm::cantFail(CPU->addressSpace()->unmap(Data + PageSize, PageSize));
        else
          llvm::cantFail(
              CPU->protect(Data + PageSize, PageSize,
                           Failure == Fault::Supervisor
                               ? Read | Write
                               : UserAccessible | (C.writes() ? Read : Write)));
        unsigned Accesses = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
        Hooks.RecoverableFault = [](const BackendFault &) { return true; };
        auto Exit = run(C.Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(Exit.Fault);
        EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
        EXPECT_EQ(Exit.Fault->Size, C.Size - Prefix);
        EXPECT_EQ(Exit.Fault->Access, C.writes() ? BackendAccessKind::Write
                                                 : BackendAccessKind::Read);
        EXPECT_EQ(Exit.Fault->Kind, Failure == Fault::Unmapped
                                        ? BackendFaultKind::UnmappedMemory
                                        : BackendFaultKind::Protection);
        expectState(State);
        EXPECT_EQ(reg(X64Register::PC), Code);
        EXPECT_EQ(word(Alias + PageSize - Prefix), Original);
        EXPECT_EQ(Accesses, 0u);
        ASSERT_TRUE(CPU->takeRecoverableFault());
        if (Failure == Fault::Unmapped)
          llvm::cantFail(CPU->mapAlias(Data + PageSize, Alias + PageSize,
                                       PageSize,
                                       Read | Write | UserAccessible));
        else
          llvm::cantFail(CPU->protect(Data + PageSize, PageSize,
                                      Read | Write | UserAccessible));
        ASSERT_EQ(run(C.Bytes).Kind, ExecutionExitKind::Stopped);
        EXPECT_EQ(reg(X64Register::PC), Code + C.Bytes.size());
        EXPECT_EQ(reg(X64Register::SI), State.SI + (C.reads() ? C.Size : 0));
        EXPECT_EQ(reg(X64Register::DI), State.DI + (C.writes() ? C.Size : 0));
      }
}

TEST_P(X64StringTransfer,
       RepeatedStopsRetainEarlierElementsAndResumeExactlyOnce) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (bool ReadStop : {false, true}) {
      if (ReadStop ? !C.reads() : !C.writes())
        continue;
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(ReadStop);
      StringState State{InitialAX, RepeatCount, Data + SourceOffset,
                        Alias + DestinationOffset, Flags};
      auto First = Before;
      StringState Host{
          InitialAX, RepeatCount,
          reinterpret_cast<uintptr_t>(First.data() + SourceOffset),
          reinterpret_cast<uintptr_t>(First.data() + DestinationOffset), Flags};
      runHost(C.Bytes, Host);
      ASSERT_FALSE(HasFatalFailure());
      seed(State);
      unsigned Elements = 0;
      BackendHooks Hooks;
      auto Stop = [&] {
        if (++Elements == 2)
          CPU->stop();
      };
      Hooks.Read = [&](uint64_t, unsigned) {
        if (ReadStop)
          Stop();
      };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
        if (!ReadStop)
          Stop();
      };
      const auto Bytes = encoding(C, true);
      auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(memory(), First);
      EXPECT_EQ(reg(X64Register::PC), Code);
      expectState({Host.AX, RepeatCount - 1,
                   State.SI + (C.reads() ? C.Size : 0),
                   State.DI + (C.writes() ? C.Size : 0), Flags});
      Host.CX = RepeatCount - 1;
      runHost(Bytes, Host);
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_EQ(run(Bytes).Kind, ExecutionExitKind::Stopped);
      EXPECT_EQ(memory(), First);
      expectState({Host.AX, 0,
                   State.SI + (C.reads() ? RepeatCount * C.Size : 0),
                   State.DI + (C.writes() ? RepeatCount * C.Size : 0), Flags});
      EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
    }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringTransfer, RepeatedFaultRetainsCompletedElements) {
  for (const auto &C : Cases) {
    resetCPU();
    ASSERT_FALSE(HasFatalFailure());
    SCOPED_TRACE(C.Name);
    const uint64_t Start = Data + PageSize - C.Size;
    StringState State{InitialAX, RepeatCount,
                      C.writes() ? Data + SourceOffset : Start,
                      C.writes() ? Start : InvalidPointer, Flags};
    seed(State);
    llvm::cantFail(CPU->writeInteger(Start, InitialAX, C.Size));
    auto Expected = C.Kind == Operation::Move
                        ? llvm::cantFail(CPU->readInteger(State.SI, C.Size))
                        : InitialAX;
    Expected &= UINT64_MAX >> ((WordBytes - C.Size) * CHAR_BIT);
    BackendHooks Hooks;
    Hooks.RecoverableFault = [](const BackendFault &) { return true; };
    auto Exit = run(encoding(C, true), std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
        << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
    EXPECT_EQ(Exit.Fault->Size, C.Size);
    EXPECT_EQ(Exit.Fault->Access,
              C.writes() ? BackendAccessKind::Write : BackendAccessKind::Read);
    EXPECT_EQ(reg(X64Register::CX), RepeatCount - 1);
    EXPECT_EQ(reg(X64Register::SI), State.SI + (C.reads() ? C.Size : 0));
    EXPECT_EQ(reg(X64Register::DI), State.DI + (C.writes() ? C.Size : 0));
    EXPECT_EQ(reg(X64Register::PC), Code);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    ASSERT_TRUE(CPU->takeRecoverableFault());
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Start, C.Size)), Expected);
  }
}

TEST_P(X64StringTransfer, ObserverExceptionsLeaveTheCurrentElementUntouched) {
  for (const auto &C : Cases)
    for (bool ReadFailure : {false, true}) {
      if (ReadFailure ? !C.reads() : !C.writes())
        continue;
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(ReadFailure);
      StringState State{InitialAX, RepeatCount, Data + SourceOffset,
                        Alias + DestinationOffset, Flags};
      seed(State);
      auto Fail = [&] {
        expectState(State);
        EXPECT_EQ(memory(), Before);
        throw std::runtime_error(ObserverException);
      };
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) {
        if (ReadFailure)
          Fail();
      };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
        if (!ReadFailure)
          Fail();
      };
      auto Exit = run(encoding(C, true), std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
      expectState(State);
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(reg(X64Register::PC), Code);
    }
}

TEST_P(X64StringTransfer, UnsupportedRepeatAndLockPrefixesFailBeforeEffects) {
  for (const auto &C : Cases)
    for (uint8_t Prefix : {uint8_t(Repne), uint8_t(Lock)}) {
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Prefix);
      StringState State{InitialAX, RepeatCount, Data + SourceOffset,
                        Alias + DestinationOffset, Flags};
      seed(State);
      std::vector<uint8_t> Bytes{Prefix};
      Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
      unsigned Accesses = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
      auto Exit = run(Bytes, std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
      EXPECT_EQ(Accesses, 0u);
      expectState(State);
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(reg(X64Register::PC), Code);
    }
}

TEST_P(X64StringTransfer, UnmodeledDeviceLoadsAndStoresRejectBeforeCallbacks) {
  for (const auto &C : Cases) {
    if (C.Kind == Operation::Move)
      continue;
    resetCPU();
    ASSERT_FALSE(HasFatalFailure());
    SCOPED_TRACE(C.Name);
    unsigned Calls = 0;
    GuestMMIOCallbacks Device;
    Device.Validate = [&](uint64_t, unsigned, bool) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Calls;
      return InitialAX;
    };
    Device.Write = [&](uint64_t, unsigned, uint64_t) {
      ++Calls;
      return llvm::Error::success();
    };
    if (GetParam().User) {
      auto E = CPU->mapMMIO(Stack, PageSize, std::move(Device));
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(Calls, 0u);
      continue;
    }
    llvm::cantFail(CPU->mapMMIO(Stack, PageSize, std::move(Device)));
    StringState State{InitialAX, RepeatCount,
                      C.reads() ? Stack : InvalidPointer,
                      C.writes() ? Stack : InvalidPointer, Flags};
    seed(State);
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Calls; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Calls; };
    auto Exit = run(encoding(C, true), std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    EXPECT_EQ(Calls, 0u);
    expectState(State);
    EXPECT_EQ(memory(), Before);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64StringTransfer,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

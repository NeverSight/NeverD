//===- X64StringComparisonTests.cpp - Restartable compare/scan checks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64StringTestSupport.h"

#if defined(__linux__) && defined(__x86_64__)
#include <atomic>
#include <cerrno>
#include <cpuid.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace neverd::emulation {
namespace {
using namespace string_test;
#define NEVERD_COMPARISON_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_COMPARISON_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_COMPARISON_BYTES(Name, ...)                                     \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_COMPARISON_CASE(Name, Size, Source, ...)                        \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StringComparisonCases.def"
#undef NEVERD_COMPARISON_CASE
#undef NEVERD_COMPARISON_BYTES
#undef NEVERD_COMPARISON_VALUE
#undef NEVERD_COMPARISON_TEXT

struct ComparisonCase {
  const char *Name;
  unsigned Size;
  bool Source;
  llvm::ArrayRef<uint8_t> Bytes;
};
const ComparisonCase Cases[] = {
#define NEVERD_COMPARISON_CASE(Name, Size, Source, ...)                        \
  {#Name, Size, Source, Name},
#include "X64StringComparisonCases.def"
#undef NEVERD_COMPARISON_CASE
};

class X64StringComparison : public X64StringTest {
protected:
  static std::vector<uint8_t> instruction(const ComparisonCase &C,
                                          uint8_t Prefix = 0,
                                          bool Narrow = false,
                                          uint8_t Segment = 0) {
    auto Bytes = encoding(C, false, Narrow, Segment);
    if (Prefix)
      Bytes.insert(Bytes.begin(), Prefix);
    return Bytes;
  }
  void fill(const ComparisonCase &C, uint64_t Source, uint64_t Destination,
            uint64_t Left = FillValue, uint64_t Right = FillValue,
            unsigned Count = RepeatCount) {
    for (unsigned I = 0; I < Count; ++I) {
      if (C.Source)
        llvm::cantFail(CPU->writeInteger(Source + I * C.Size, Left, C.Size));
      llvm::cantFail(
          CPU->writeInteger(Destination + I * C.Size, Right, C.Size));
    }
  }
  static uint64_t hostFlags(const ComparisonCase &C, uint64_t Left,
                            uint64_t Right, uint64_t InputFlags) {
    StringState Host{Left, 0, reinterpret_cast<uintptr_t>(&Left),
                     reinterpret_cast<uintptr_t>(&Right), InputFlags};
    runHost(C.Bytes, Host);
    return Host.Flags;
  }
};

TEST_P(X64StringComparison,
       AllArithmeticFlagsMatchIndependentHostInstructions) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases) {
    const uint64_t Mask = UINT64_MAX >> ((WordBytes - C.Size) * CHAR_BIT);
    const uint64_t Sign = (Mask >> 1) + 1;
    for (uint64_t Left : {uint64_t(0), uint64_t(1), AuxiliaryBoundary - 1,
                          AuxiliaryBoundary, Sign - 1, Sign, Mask})
      for (uint64_t Right : {uint64_t(0), uint64_t(1), AuxiliaryBoundary - 1,
                             AuxiliaryBoundary, Sign - 1, Sign, Mask})
        for (uint64_t InputFlags : {PlainFlags, AllFlags}) {
          SCOPED_TRACE(C.Name);
          SCOPED_TRACE(Left);
          SCOPED_TRACE(Right);
          const StringState State{(InitialAX & ~Mask) | Left, RepeatCount,
                                  C.Source ? Data + SourceOffset
                                           : InvalidPointer,
                                  Alias + DestinationOffset, InputFlags};
          seed(State);
          fill(C, State.SI, State.DI, Left, Right, 1);
          const auto Original = memory();
          const auto ExpectedFlags = hostFlags(C, Left, Right, InputFlags);
          ASSERT_FALSE(HasFatalFailure());
          auto Exit = run(C.Bytes);
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          const uint64_t Delta =
              InputFlags & Direction ? uint64_t(0) - C.Size : C.Size;
          expectState({State.AX, State.CX, State.SI + (C.Source ? Delta : 0),
                       State.DI + Delta, ExpectedFlags});
          EXPECT_EQ(memory(), Original);
          EXPECT_EQ(reg(X64Register::PC), Code + C.Bytes.size());
        }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringComparison, ConditionalRepeatsMatchHostAndStopAtFirstResult) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (bool Backward : {false, true})
        for (unsigned MatchAt = 0; MatchAt <= RepeatCount; ++MatchAt)
          for (uint64_t InputFlags : {PlainFlags, AllFlags & ~Direction}) {
            SCOPED_TRACE(C.Name);
            SCOPED_TRACE(Prefix);
            SCOPED_TRACE(MatchAt);
            const uint64_t Offset = Backward ? (RepeatCount - 1) * C.Size : 0;
            StringState State{FillValue, RepeatCount,
                              C.Source ? Data + SourceOffset + Offset
                                       : InvalidPointer,
                              Alias + DestinationOffset + Offset,
                              InputFlags | (Backward ? Direction : 0)};
            seed(State);
            const uint64_t Right = Prefix == Rep ? FillValue : DifferentValue;
            fill(C, Data + SourceOffset, Alias + DestinationOffset, FillValue,
                 Right);
            if (MatchAt < RepeatCount) {
              const auto Index = Backward ? RepeatCount - 1 - MatchAt : MatchAt;
              llvm::cantFail(CPU->writeInteger(
                  Alias + DestinationOffset + Index * C.Size,
                  Prefix == Rep ? DifferentValue : FillValue, C.Size));
            }
            auto Expected = memory();
            StringState Host{State.AX, State.CX,
                             C.Source
                                 ? reinterpret_cast<uintptr_t>(
                                       Expected.data() + SourceOffset + Offset)
                                 : InvalidPointer,
                             reinterpret_cast<uintptr_t>(
                                 Expected.data() + DestinationOffset + Offset),
                             State.Flags};
            const auto OriginalHost = Host;
            const auto Bytes = instruction(C, Prefix);
            runHost(Bytes, Host);
            ASSERT_FALSE(HasFatalFailure());
            unsigned Reads = 0, Writes = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t Address, unsigned Size) {
              const auto Element = Reads / (C.Source ? 2 : 1);
              const uint64_t Delta = Element * C.Size;
              const uint64_t Start =
                  C.Source && Reads % 2 == 0 ? State.SI : State.DI;
              EXPECT_EQ(Address, Backward ? Start - Delta : Start + Delta);
              EXPECT_EQ(Size, C.Size);
              ++Reads;
            };
            Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
            auto Exit = run(Bytes, std::move(Hooks));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
            expectState({Host.AX, Host.CX, State.SI + Host.SI - OriginalHost.SI,
                         State.DI + Host.DI - OriginalHost.DI, Host.Flags});
            EXPECT_EQ(Reads, (RepeatCount - Host.CX) * (C.Source ? 2 : 1));
            EXPECT_EQ(Writes, 0u);
            EXPECT_EQ(memory(), Expected);
            EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
          }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringComparison, ZeroCountDoesNotReadPointersOrChangeFlags) {
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (bool Narrow : {false, true}) {
        const StringState State{
            InitialAX, 0, Narrow ? SourceOffset : InvalidPointer,
            Narrow ? DestinationOffset : InvalidPointer, AllFlags};
        seed(State);
        unsigned Reads = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
        auto Bytes = instruction(C, Prefix, Narrow);
        auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        expectState(State);
        EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
        EXPECT_EQ(Reads, 0u);
      }
}

TEST_P(X64StringComparison,
       NarrowSourceWrapsBeforeSegmentBaseAndScanIgnoresSI) {
  llvm::cantFail(CPU->mapAlias(SegmentBase + Data, Data, PageSize,
                               Read | Write | UserAccessible));
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (uint8_t Segment : {FSPrefix, GSPrefix}) {
        const auto SegmentRegister = Segment == FSPrefix
                                         ? CPURegister::X64FSBase
                                         : CPURegister::X64GSBase;
        llvm::cantFail(CPU->writeRegister(SegmentRegister, {SegmentBase, 0}));
        const StringState State{
            FillValue, UpperBits | RepeatCount,
            C.Source ? UpperBits | (Data + SourceOffset) : InvalidPointer,
            UpperBits | (Alias + DestinationOffset), AllFlags & ~Direction};
        seed(State);
        fill(C, Data + SourceOffset, Alias + DestinationOffset, FillValue,
             Prefix == Rep ? FillValue : DifferentValue);
        unsigned Reads = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t Address, unsigned) {
          const auto Element = Reads / (C.Source ? 2 : 1);
          const auto Start = C.Source && Reads % 2 == 0
                                 ? SegmentBase + Data + SourceOffset
                                 : Alias + DestinationOffset;
          EXPECT_EQ(Address, Start + Element * C.Size);
          ++Reads;
        };
        auto Exit =
            run(instruction(C, Prefix, true, Segment), std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(reg(X64Register::CX), 0u);
        EXPECT_EQ(reg(X64Register::SI),
                  C.Source ? Data + SourceOffset + RepeatCount * C.Size
                           : State.SI);
        EXPECT_EQ(reg(X64Register::DI),
                  Alias + DestinationOffset + RepeatCount * C.Size);
        EXPECT_EQ(reg(X64Register::AX), State.AX);
        EXPECT_EQ(Reads, RepeatCount * (C.Source ? 2 : 1));
        llvm::cantFail(CPU->writeRegister(SegmentRegister, {0, 0}));
      }
}

TEST_P(X64StringComparison, EarlyTerminationNeverTouchesTheNextUnmappedPage) {
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne}) {
      const auto Destination = Alias + PageSize - C.Size;
      StringState State{FillValue, RepeatCount,
                        C.Source ? Data + SourceOffset : InvalidPointer,
                        Destination, PlainFlags};
      seed(State);
      fill(C, State.SI, Destination, FillValue,
           Prefix == Rep ? DifferentValue : FillValue, 1);
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      const auto Bytes = instruction(C, Prefix);
      auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(reg(X64Register::CX), RepeatCount - 1);
      EXPECT_EQ(reg(X64Register::DI), Alias + PageSize);
      EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
      EXPECT_EQ(Reads, C.Source ? 2u : 1u);
    }
}

TEST_P(X64StringComparison, CompleteOperandsAreValidatedBeforeAnyReadObserver) {
  enum class Failure { Unmapped, Permission, Supervisor };
  mapCrossingPages();
  for (const auto &C : Cases)
    for (bool SourceFault : {false, true}) {
      if (SourceFault && !C.Source)
        continue;
      for (unsigned Prefix = 1; Prefix < C.Size; ++Prefix)
        for (auto Failure :
             {Failure::Unmapped, Failure::Permission, Failure::Supervisor}) {
          if (Failure == Failure::Supervisor && !GetParam().User)
            continue;
          const auto Crossing = Data + PageSize - Prefix;
          StringState State{FillValue, RepeatCount,
                            SourceFault ? Crossing : Data + SourceOffset,
                            SourceFault ? Alias + DestinationOffset : Crossing,
                            PlainFlags};
          seed(State);
          fill(C, State.SI, State.DI, FillValue, FillValue, 1);
          const auto Original = word(Alias + PageSize - Prefix);
          if (Failure == Failure::Unmapped)
            llvm::cantFail(
                CPU->addressSpace()->unmap(Data + PageSize, PageSize));
          else
            llvm::cantFail(CPU->protect(Data + PageSize, PageSize,
                                        Failure == Failure::Supervisor
                                            ? Read | Write
                                            : Write | UserAccessible));
          unsigned Reads = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
          Hooks.RecoverableFault = [&](const BackendFault &) {
            expectState(State);
            return true;
          };
          auto Exit = run(instruction(C, Rep), std::move(Hooks));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
              << Exit.Diagnostic;
          ASSERT_TRUE(Exit.Fault);
          EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
          EXPECT_EQ(Exit.Fault->Size, C.Size - Prefix);
          EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
          EXPECT_EQ(Reads, 0u);
          expectState(State);
          EXPECT_EQ(word(Alias + PageSize - Prefix), Original);
          ASSERT_TRUE(CPU->takeRecoverableFault());
          if (Failure == Failure::Unmapped)
            llvm::cantFail(CPU->mapAlias(Data + PageSize, Alias + PageSize,
                                         PageSize,
                                         Read | Write | UserAccessible));
          else
            llvm::cantFail(CPU->protect(Data + PageSize, PageSize,
                                        Read | Write | UserAccessible));
          ASSERT_EQ(run(C.Bytes).Kind, ExecutionExitKind::Stopped);
          EXPECT_EQ(reg(X64Register::FLAGS) & ZeroFlag, ZeroFlag);
        }
    }
}

TEST_P(X64StringComparison,
       LaterFaultRestoresEntryFlagsAndKeepsCompletedElements) {
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (bool Recoverable : {false, true})
        for (bool SourceFault : {false, true}) {
          if (SourceFault && !C.Source)
            continue;
          resetCPU();
          ASSERT_FALSE(HasFatalFailure());
          const auto Crossing = Data + PageSize - C.Size;
          StringState State{FillValue, RepeatCount,
                            SourceFault ? Crossing : Data + SourceOffset,
                            SourceFault ? Alias + DestinationOffset : Crossing,
                            Prefix == Rep ? PlainFlags
                                          : (AllFlags & ~Direction)};
          seed(State);
          const auto Right = Prefix == Rep ? FillValue : DifferentValue;
          fill(C, State.SI, State.DI, FillValue, Right, 1);
          const StringState Expected{State.AX, RepeatCount - 1,
                                     State.SI + (C.Source ? C.Size : 0),
                                     State.DI + C.Size, State.Flags};
          unsigned Reads = 0, Faults = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
          Hooks.RecoverableFault = [&](const BackendFault &) {
            ++Faults;
            expectState(Expected);
            return Recoverable;
          };
          Hooks.Fault = [&](uint64_t, unsigned, llvm::StringRef) {
            expectState(Expected);
          };
          const auto Bytes = instruction(C, Prefix);
          auto Exit = run(Bytes, std::move(Hooks));
          ASSERT_EQ(Exit.Kind, Recoverable ? ExecutionExitKind::RecoverableFault
                                           : ExecutionExitKind::GuestFault)
              << Exit.Diagnostic;
          EXPECT_EQ(Reads, C.Source ? 2u : 1u);
          EXPECT_EQ(Faults, 1u);
          expectState(Expected);
          EXPECT_EQ(reg(X64Register::PC), Code);
          if (!Recoverable)
            continue;
          ASSERT_TRUE(CPU->takeRecoverableFault());
          llvm::cantFail(CPU->map(Data + PageSize, PageSize,
                                  Read | Write | UserAccessible));
          fill(C, Expected.SI, Expected.DI, FillValue, Right, RepeatCount - 1);
          ASSERT_EQ(run(Bytes).Kind, ExecutionExitKind::Stopped);
          EXPECT_EQ(reg(X64Register::CX), 0u);
          EXPECT_EQ(reg(X64Register::DI), State.DI + RepeatCount * C.Size);
          EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
        }
}

TEST_P(X64StringComparison,
       StopsRetainLastComparisonAndResumeFromSavedContext) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (unsigned StopAt = 0; StopAt < (C.Source ? 2u : 1u); ++StopAt) {
        StringState State{FillValue, RepeatCount,
                          C.Source ? Data + SourceOffset : InvalidPointer,
                          Alias + DestinationOffset, PlainFlags};
        seed(State);
        const auto Right = Prefix == Rep ? FillValue : DifferentValue;
        fill(C, State.SI, State.DI, FillValue, Right);
        const auto ExpectedFlags = hostFlags(C, FillValue, Right, State.Flags);
        unsigned Reads = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) {
          if (Reads++ == (C.Source ? 2u : 1u) + StopAt)
            CPU->stop();
        };
        const auto Bytes = instruction(C, Prefix);
        auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        const StringState Expected{State.AX, RepeatCount - 1,
                                   State.SI + (C.Source ? C.Size : 0),
                                   State.DI + C.Size, ExpectedFlags};
        expectState(Expected);
        EXPECT_EQ(reg(X64Register::PC), Code);
        auto Snapshot = llvm::cantFail(CPU->saveContext());
        ASSERT_EQ(run(Bytes).Kind, ExecutionExitKind::Stopped);
        const auto FinalFlags = reg(X64Register::FLAGS);
        llvm::cantFail(CPU->restoreContext(*Snapshot));
        expectState(Expected);
        ASSERT_EQ(run(Bytes).Kind, ExecutionExitKind::Stopped);
        EXPECT_EQ(reg(X64Register::CX), 0u);
        EXPECT_EQ(reg(X64Register::DI), State.DI + RepeatCount * C.Size);
        EXPECT_EQ(reg(X64Register::FLAGS), FinalFlags);
      }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringComparison, ObserverFailureDoesNotPublishCurrentComparison) {
  for (const auto &C : Cases)
    for (unsigned FailureAt = 0; FailureAt < (C.Source ? 2u : 1u);
         ++FailureAt) {
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      StringState State{FillValue, RepeatCount,
                        C.Source ? Data + SourceOffset : InvalidPointer,
                        Alias + DestinationOffset, PlainFlags};
      seed(State);
      fill(C, State.SI, State.DI);
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) {
        if (Reads++ == FailureAt)
          throw std::runtime_error(ObserverException);
      };
      auto Exit = run(instruction(C, Rep), std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
      expectState(State);
      EXPECT_EQ(reg(X64Register::PC), Code);
    }
}

TEST_P(X64StringComparison,
       LockedStringsRejectAndScalarSSEPreservesStringOperands) {
  for (const auto &C : Cases)
    for (bool SSE : {false, true}) {
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      StringState State{FillValue, RepeatCount, Data + SourceOffset,
                        Alias + DestinationOffset, PlainFlags};
      seed(State);
      llvm::cantFail(CPU->setXmm(0, {0, FillValue}));
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      const auto Bytes = instruction(C, Lock);
      auto Exit =
          run(SSE ? llvm::ArrayRef(ScalarSSECompare) : llvm::ArrayRef(Bytes),
              std::move(Hooks));
      EXPECT_EQ(Exit.Kind, SSE ? ExecutionExitKind::Stopped
                               : ExecutionExitKind::UnsupportedOperation);
      EXPECT_EQ(llvm::cantFail(CPU->xmm(0)),
                (RegisterValue{SSE ? UINT64_MAX : 0, FillValue}));
      expectState(State);
      EXPECT_EQ(Reads, 0u);
    }
}

TEST_P(X64StringComparison, CrossPageReadOnlyOperandsMatchHostThroughAliases) {
#if defined(__x86_64__) || defined(_M_X64)
  mapCrossingPages();
  for (const auto &C : Cases)
    for (unsigned Offset = 1; Offset < C.Size; ++Offset)
      for (bool SourceCrossing : {false, true}) {
        if (SourceCrossing && !C.Source)
          continue;
        StringState State{FillValue, RepeatCount,
                          SourceCrossing ? Data + PageSize - Offset
                                         : Data + SourceOffset,
                          SourceCrossing ? Alias + DestinationOffset
                                         : Alias + PageSize - Offset,
                          AllFlags & ~Direction};
        seed(State);
        fill(C, State.SI, State.DI, FillValue, DifferentValue, 1);
        const auto ExpectedFlags =
            hostFlags(C, FillValue, DifferentValue, State.Flags);
        llvm::cantFail(CPU->protect(Data, 2 * PageSize, Read | UserAccessible));
        llvm::cantFail(
            CPU->protect(Alias, 2 * PageSize, Read | UserAccessible));
        auto Exit = run(C.Bytes);
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        expectState({State.AX, State.CX, State.SI + (C.Source ? C.Size : 0),
                     State.DI + C.Size, ExpectedFlags});
        llvm::cantFail(
            CPU->protect(Data, 2 * PageSize, Read | Write | UserAccessible));
        llvm::cantFail(
            CPU->protect(Alias, 2 * PageSize, Read | Write | UserAccessible));
      }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringComparison,
       AddressSizeWrappingRetainsConditionalRepeatProgress) {
  llvm::cantFail(
      CPU->map(LastNarrowPage, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->map(0, PageSize, Read | Write | UserAccessible));
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (bool Backward : {false, true}) {
        const uint64_t Start =
            Backward ? 0 : LastNarrowPage + PageSize - C.Size;
        StringState State{FillValue, UpperBits | RepeatCount,
                          C.Source ? UpperBits | Start : InvalidPointer,
                          UpperBits | Start,
                          PlainFlags | (Backward ? Direction : 0)};
        seed(State);
        const uint64_t Delta = Backward ? uint64_t(0) - C.Size : C.Size;
        for (unsigned I = 0; I < RepeatCount; ++I)
          llvm::cantFail(CPU->writeInteger(
              uint32_t(Start + I * Delta),
              !C.Source && Prefix == Repne ? DifferentValue : FillValue,
              C.Size));
        const auto Bytes = instruction(C, Prefix, true);
        auto Exit = run(Bytes);
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        const auto Elements = C.Source && Prefix == Repne ? 1 : RepeatCount;
        EXPECT_EQ(reg(X64Register::CX), RepeatCount - Elements);
        EXPECT_EQ(reg(X64Register::SI),
                  C.Source ? uint32_t(Start + Elements * Delta) : State.SI);
        EXPECT_EQ(reg(X64Register::DI), uint32_t(Start + Elements * Delta));
        EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
      }
}

TEST_P(X64StringComparison, AResumedFaultUsesTheNewEntryFlags) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne}) {
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      StringState State{FillValue, RepeatCount,
                        C.Source ? Data + SourceOffset : InvalidPointer,
                        Alias + PageSize - C.Size, PlainFlags};
      seed(State);
      const auto Right = Prefix == Rep ? FillValue : DifferentValue;
      fill(C, State.SI, State.DI, FillValue, Right, 1);
      const auto ExpectedFlags = hostFlags(C, FillValue, Right, State.Flags);
      unsigned Iterations = 0;
      BackendHooks Stop;
      Stop.Instruction = [&](uint64_t, unsigned) {
        if (++Iterations == 2)
          CPU->stop();
      };
      const auto Bytes = instruction(C, Prefix);
      ASSERT_EQ(run(Bytes, std::move(Stop)).Kind, ExecutionExitKind::Stopped);
      EXPECT_EQ(reg(X64Register::FLAGS), ExpectedFlags);
      auto Snapshot = llvm::cantFail(CPU->saveContext());
      // Running a different instruction must not keep a previous REP's flag
      // checkpoint, and restoring the architectural context starts a new REP.
      ASSERT_EQ(run(ClearDirection).Kind, ExecutionExitKind::Stopped);
      llvm::cantFail(CPU->restoreContext(*Snapshot));
      BackendHooks Fault;
      Fault.RecoverableFault = [&](const BackendFault &) {
        EXPECT_EQ(reg(X64Register::FLAGS), ExpectedFlags);
        return true;
      };
      ASSERT_EQ(run(Bytes, std::move(Fault)).Kind,
                ExecutionExitKind::RecoverableFault);
      EXPECT_EQ(reg(X64Register::FLAGS), ExpectedFlags);
      EXPECT_EQ(reg(X64Register::CX), RepeatCount - 1);
      ASSERT_TRUE(CPU->takeRecoverableFault());
    }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64StringComparison, AmbiguousInactiveUpperHalvesRejectBeforeEffects) {
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (auto Register :
           {X64Register::CX, X64Register::SI, X64Register::DI}) {
        if (!C.Source && Register == X64Register::SI)
          continue;
        resetCPU();
        ASSERT_FALSE(HasFatalFailure());
        StringState State{InitialAX, 0, SourceOffset, DestinationOffset,
                          AllFlags};
        if (Register == X64Register::CX)
          State.CX |= UpperBits;
        else if (Register == X64Register::SI)
          State.SI |= UpperBits;
        else
          State.DI |= UpperBits;
        seed(State);
        unsigned Reads = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
        auto Exit = run(instruction(C, Prefix, true), std::move(Hooks));
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
        expectState(State);
        EXPECT_EQ(Reads, 0u);
      }
}

TEST_P(X64StringComparison, DeviceOperandsRejectBeforeCallbacksOrObservations) {
  for (const auto &C : Cases)
    for (bool SourceDevice : {false, true}) {
      if (SourceDevice && !C.Source)
        continue;
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      unsigned Calls = 0;
      GuestMMIOCallbacks Device;
      Device.Validate = [&](uint64_t, unsigned, bool) {
        ++Calls;
        return llvm::Error::success();
      };
      Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
        ++Calls;
        return FillValue;
      };
      Device.Write = [&](uint64_t, unsigned, uint64_t) {
        ++Calls;
        return llvm::Error::success();
      };
      auto Mapped = CPU->mapMMIO(Stack, PageSize, std::move(Device));
      if (GetParam().User) {
        EXPECT_TRUE(bool(Mapped));
        llvm::consumeError(std::move(Mapped));
        EXPECT_EQ(Calls, 0u);
        continue;
      }
      llvm::cantFail(std::move(Mapped));
      StringState State{
          FillValue, RepeatCount, SourceDevice ? Stack : Data + SourceOffset,
          SourceDevice ? Alias + DestinationOffset : Stack, PlainFlags};
      seed(State);
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      auto Exit = run(instruction(C, Rep), std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
      expectState(State);
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Calls, 0u);
    }
}

#if defined(__linux__) && defined(__x86_64__)
struct NativeComparisonFault {
  std::atomic<uint64_t> AX{0}, CX{0}, SI{0}, DI{0}, Flags{0}, Address{0};
  std::atomic<int> Signal{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<NativeComparisonFault *>::is_always_lock_free);
static_assert(sizeof(NativeComparisonFault) <= PageSize);
std::atomic<NativeComparisonFault *> NativeRecord{nullptr};
void recordComparisonFault(int Signal, siginfo_t *Info, void *Context) {
  auto *Record = NativeRecord.load(std::memory_order_relaxed);
  if (!Record)
    _exit(SetupFailure);
  const auto &Registers = static_cast<ucontext_t *>(Context)->uc_mcontext.gregs;
  Record->AX.store(Registers[REG_RAX], std::memory_order_relaxed);
  Record->CX.store(Registers[REG_RCX], std::memory_order_relaxed);
  Record->SI.store(Registers[REG_RSI], std::memory_order_relaxed);
  Record->DI.store(Registers[REG_RDI], std::memory_order_relaxed);
  Record->Flags.store(Registers[REG_EFL], std::memory_order_relaxed);
  Record->Address.store(reinterpret_cast<uintptr_t>(Info->si_addr),
                        std::memory_order_relaxed);
  Record->Signal.store(Signal, std::memory_order_relaxed);
  _exit(FaultExit);
}
#endif

TEST(X64StringComparisonOracle,
     NativePageFaultPreservesVendorFlagsAndRetainsProgress) {
#if defined(__linux__) && defined(__x86_64__)
  unsigned EAX, EBX, ECX, EDX;
  ASSERT_TRUE(__get_cpuid(OracleVendorLeaf, &EAX, &EBX, &ECX, &EDX));
  const unsigned VendorWords[] = {EBX, EDX, ECX};
  const llvm::StringRef Vendor(reinterpret_cast<const char *>(VendorWords),
                               sizeof(VendorWords));
  std::optional<bool> RestoreFlags;
#define NEVERD_COMPARISON_HOST(Name, VendorName, Restore)                      \
  if (Vendor == VendorName)                                                    \
    RestoreFlags = Restore;
#include "X64StringComparisonCases.def"
#undef NEVERD_COMPARISON_HOST
  ASSERT_TRUE(RestoreFlags.has_value()) << Vendor.str();
  SCOPED_TRACE(Vendor.str());
  for (const auto &C : Cases)
    for (uint8_t Prefix : {Rep, Repne})
      for (bool CompletedFirst : {false, true})
        for (bool SourceFault : {false, true}) {
          if (SourceFault && !C.Source)
            continue;
          SCOPED_TRACE(C.Name);
          SCOPED_TRACE(Prefix);
          SCOPED_TRACE(CompletedFirst);
          SCOPED_TRACE(SourceFault);
          auto *Storage = static_cast<uint8_t *>(
              mmap(nullptr, OraclePages * PageSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0));
          ASSERT_NE(Storage, MAP_FAILED);
          auto Release = llvm::scope_exit(
              [&] { munmap(Storage, OraclePages * PageSize); });
          auto *Record = new (Storage + 2 * PageSize) NativeComparisonFault;
          const auto ProgressBytes = CompletedFirst ? C.Size : 0;
          auto *Source =
              Storage + (SourceFault ? PageSize - ProgressBytes : SourceOffset);
          auto *Destination =
              Storage +
              (SourceFault ? DestinationOffset : PageSize - ProgressBytes);
          const uint64_t Left = FillValue,
                         Right = Prefix == Rep ? FillValue : DifferentValue;
          if (C.Source)
            std::memcpy(Source, &Left, C.Size);
          std::memcpy(Destination, &Right, C.Size);
          const uint64_t InputFlags =
              Prefix == Rep ? PlainFlags : (AllFlags & ~Direction);
          // A non-repeated native comparison independently supplies AMD's flags
          // after one completed element. Faulting the first element keeps the
          // input flags on either vendor; an unknown vendor is not guessed.
          StringState Single{Left, 0, reinterpret_cast<uintptr_t>(&Left),
                             reinterpret_cast<uintptr_t>(&Right), InputFlags};
          runHost(C.Bytes, Single);
          ASSERT_FALSE(HasFatalFailure());
          const uint64_t ExpectedFlags =
              *RestoreFlags || !CompletedFirst ? InputFlags : Single.Flags;
          StringState State{
              FillValue, RepeatCount,
              C.Source ? reinterpret_cast<uintptr_t>(Source) : InvalidPointer,
              reinterpret_cast<uintptr_t>(Destination), InputFlags};
          const pid_t Child = fork();
          ASSERT_GE(Child, 0);
          if (!Child) {
            NativeRecord.store(Record, std::memory_order_relaxed);
            struct sigaction Action{};
            Action.sa_sigaction = recordComparisonFault;
            Action.sa_flags = SA_SIGINFO;
            sigemptyset(&Action.sa_mask);
            sigset_t Signals;
            sigemptyset(&Signals);
            sigaddset(&Signals, SIGSEGV);
            if (sigaction(SIGSEGV, &Action, nullptr) ||
                sigprocmask(SIG_UNBLOCK, &Signals, nullptr) ||
                mprotect(Storage + PageSize, PageSize, PROT_NONE))
              _exit(SetupFailure);
            std::vector<uint8_t> Bytes{Prefix};
            Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
            runHost(Bytes, State);
            _exit(SetupFailure);
          }
          int Status = 0;
          pid_t Waited;
          do {
            Waited = waitpid(Child, &Status, 0);
          } while (Waited < 0 && errno == EINTR);
          ASSERT_EQ(Waited, Child);
          ASSERT_TRUE(WIFEXITED(Status));
          ASSERT_EQ(WEXITSTATUS(Status), FaultExit);
          EXPECT_EQ(Record->Signal.load(), SIGSEGV);
          EXPECT_EQ(Record->Address.load(),
                    reinterpret_cast<uintptr_t>(Storage + PageSize));
          EXPECT_EQ(Record->AX.load(), State.AX);
          EXPECT_EQ(Record->CX.load(), RepeatCount - unsigned(CompletedFirst));
          EXPECT_EQ(Record->SI.load(),
                    State.SI + (C.Source ? ProgressBytes : 0));
          EXPECT_EQ(Record->DI.load(), State.DI + ProgressBytes);
          // The host adds RF when reporting the synchronous fault.
          EXPECT_EQ(Record->Flags.load() & ~OracleResumeFlag, ExpectedFlags);
        }
#else
  GTEST_SKIP() << FaultOracleUnavailable;
#endif
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64StringComparison,
                         testing::ValuesIn(Parameters),
                         [](const auto &P) { return P.param.Name; });
} // namespace
} // namespace neverd::emulation

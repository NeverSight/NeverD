//===- X64ShuffleTests.cpp - Legacy SSE lane and fault semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/APInt.h"

#include <climits>
#include <stdexcept>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_SHUFFLE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SHUFFLE_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_SHUFFLE_INVALID(Name, ...)                                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INVALID
#undef NEVERD_SHUFFLE_TEXT
#undef NEVERD_SHUFFLE_VALUE
enum class Operation {
#define NEVERD_SHUFFLE_OPERATION(Name, ...) Name,
#define NEVERD_SHUFFLE_INTERLEAVE(Name, ...) Name,
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INTERLEAVE
#undef NEVERD_SHUFFLE_OPERATION
};
struct Shuffle {
  const char *Name;
  Operation Kind;
  uint8_t Prefix, Opcode;
  bool HasControl;
};
constexpr Shuffle Shuffles[] = {
#define NEVERD_SHUFFLE_OPERATION(Name, Prefix, Opcode)                         \
  {#Name, Operation::Name, Prefix, Opcode, true},
#define NEVERD_SHUFFLE_INTERLEAVE(Name, Prefix, Opcode)                        \
  {#Name, Operation::Name, Prefix, Opcode, false},
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INTERLEAVE
#undef NEVERD_SHUFFLE_OPERATION
};
constexpr Input Inputs[] = {
#define NEVERD_SHUFFLE_INPUT(Low, High, SourceLow, SourceHigh)                 \
  {{Low, High}, {SourceLow, SourceHigh}},
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INPUT
};
constexpr uint64_t MemoryAddress = Alias + PageSize - VectorBytes;

RegisterValue scalarResult(Operation Op, const Input &I, unsigned Control) {
  const llvm::APInt Left(VectorBytes * CHAR_BIT, llvm::ArrayRef(I.Left));
  const llvm::APInt Right(VectorBytes * CHAR_BIT, llvm::ArrayRef(I.Right));
  auto Result = Right;
  switch (Op) {
  case Operation::PSHUFD:
  case Operation::SHUFPS:
    for (unsigned Lane = 0; Lane < VectorBytes / sizeof(uint32_t); ++Lane) {
      const auto &Source =
          Op == Operation::SHUFPS && Lane < WordBytes / sizeof(uint32_t)
              ? Left
              : Right;
      const unsigned Index = (Control >> (Lane * ControlBits)) & ControlMask;
      Result.insertBits(Source.extractBits(sizeof(uint32_t) * CHAR_BIT,
                                           Index * sizeof(uint32_t) * CHAR_BIT),
                        Lane * sizeof(uint32_t) * CHAR_BIT);
    }
    break;
  case Operation::PSHUFHW:
  case Operation::PSHUFLW: {
    const unsigned Base = Op == Operation::PSHUFHW ? WordBytes * CHAR_BIT : 0;
    for (unsigned Lane = 0; Lane < WordBytes / sizeof(uint16_t); ++Lane) {
      const unsigned Index = (Control >> (Lane * ControlBits)) & ControlMask;
      Result.insertBits(
          Right.extractBits(sizeof(uint16_t) * CHAR_BIT,
                            Base + Index * sizeof(uint16_t) * CHAR_BIT),
          Base + Lane * sizeof(uint16_t) * CHAR_BIT);
    }
    break;
  }
  case Operation::SHUFPD:
    return {I.Left[Control & 1], I.Right[(Control >> 1) & 1]};
  case Operation::UNPCKLPS:
  case Operation::UNPCKHPS: {
    const unsigned Base = Op == Operation::UNPCKHPS ? WordBytes * CHAR_BIT : 0;
    for (unsigned Lane = 0; Lane < WordBytes / sizeof(uint32_t); ++Lane) {
      const unsigned SourceBit = Base + Lane * sizeof(uint32_t) * CHAR_BIT;
      const unsigned DestinationBit = Lane * WordBytes * CHAR_BIT;
      Result.insertBits(
          Left.extractBits(sizeof(uint32_t) * CHAR_BIT, SourceBit),
          DestinationBit);
      Result.insertBits(
          Right.extractBits(sizeof(uint32_t) * CHAR_BIT, SourceBit),
          DestinationBit + sizeof(uint32_t) * CHAR_BIT);
    }
    break;
  }
  case Operation::UNPCKLPD:
    return {I.Left[0], I.Right[0]};
  case Operation::UNPCKHPD:
    return {I.Left[1], I.Right[1]};
  }
  return {Result.getRawData()[0], Result.getRawData()[1]};
}

#if defined(__x86_64__) || defined(_M_X64)
template <size_t Control>
RegisterValue nativeResult(Operation Op, const Input &I) {
  const auto Left =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Left.data()));
  const auto Right =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Right.data()));
  __m128i Result{};
  switch (Op) {
  case Operation::PSHUFD:
    Result = _mm_shuffle_epi32(Right, Control);
    break;
  case Operation::PSHUFHW:
    Result = _mm_shufflehi_epi16(Right, Control);
    break;
  case Operation::PSHUFLW:
    Result = _mm_shufflelo_epi16(Right, Control);
    break;
  case Operation::SHUFPS:
    Result = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(Left),
                                             _mm_castsi128_ps(Right), Control));
    break;
  case Operation::SHUFPD:
    Result = _mm_castpd_si128(_mm_shuffle_pd(_mm_castsi128_pd(Left),
                                             _mm_castsi128_pd(Right),
                                             Control & ControlMask));
    break;
  case Operation::UNPCKLPS:
    Result = _mm_castps_si128(
        _mm_unpacklo_ps(_mm_castsi128_ps(Left), _mm_castsi128_ps(Right)));
    break;
  case Operation::UNPCKHPS:
    Result = _mm_castps_si128(
        _mm_unpackhi_ps(_mm_castsi128_ps(Left), _mm_castsi128_ps(Right)));
    break;
  case Operation::UNPCKLPD:
    Result = _mm_castpd_si128(
        _mm_unpacklo_pd(_mm_castsi128_pd(Left), _mm_castsi128_pd(Right)));
    break;
  case Operation::UNPCKHPD:
    Result = _mm_castpd_si128(
        _mm_unpackhi_pd(_mm_castsi128_pd(Left), _mm_castsi128_pd(Right)));
    break;
  }
  RegisterValue Actual;
  _mm_storeu_si128(reinterpret_cast<__m128i *>(Actual.data()), Result);
  return Actual;
}
template <size_t... Control>
constexpr auto nativeFunctions(std::index_sequence<Control...>) {
  return std::array{&nativeResult<Control>...};
}
#endif

TEST(X64ShuffleOracle, AllControlsMatchScalarLaneSelection) {
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto Functions =
      nativeFunctions(std::make_index_sequence<unsigned(UINT8_MAX) + 1>{});
  for (const auto &S : Shuffles)
    for (const auto &I : Inputs)
      for (unsigned Control = 0;
           Control <= (S.HasControl ? unsigned(UINT8_MAX) : 0u); ++Control) {
        SCOPED_TRACE(S.Name);
        SCOPED_TRACE(Control);
        EXPECT_EQ(Functions[Control](S.Kind, I),
                  scalarResult(S.Kind, I, Control));
      }
#else
  GTEST_SKIP();
#endif
}

std::vector<uint8_t> instruction(const Shuffle &S, unsigned Control,
                                 unsigned Destination = 0, unsigned Source = 1,
                                 bool Memory = false) {
  std::vector<uint8_t> Bytes;
  if (S.Prefix)
    Bytes.push_back(S.Prefix);
  const uint8_t Extension =
      (Destination > RegisterFieldMask ? RexDestination : 0) |
      (!Memory && Source > RegisterFieldMask ? RexSource : 0);
  if (Extension)
    Bytes.push_back(Rex | Extension);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(S.Opcode);
  Bytes.push_back(
      ((Destination & RegisterFieldMask) << RegisterFieldBits) |
      (Memory ? MemoryModRM : RegisterModRM | (Source & RegisterFieldMask)));
  if (S.HasControl)
    Bytes.push_back(Control);
  return Bytes;
}

class X64Shuffle : public X64VectorTest {
protected:
  void initialize(const Input &I, unsigned Destination = 0, unsigned Source = 1,
                  uint64_t Address = MemoryAddress) {
    seed(I, Address);
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setXmm(Destination, I.Left));
    if (Destination != Source)
      llvm::cantFail(CPU->setXmm(Source, I.Right));
    llvm::cantFail(CPU->writeInteger(MemoryAddress, I.Right[0], WordBytes));
    llvm::cantFail(
        CPU->writeInteger(MemoryAddress + WordBytes, I.Right[1], WordBytes));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FP0,
                                      {SentinelLow, PhysicalExponent}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FPTag, {1, 0}));
  }
  auto backing() {
    std::array<uint8_t, PageSize> Bytes{};
    llvm::cantFail(CPU->snapshotBacking(Alias, Bytes));
    return Bytes;
  }
  void check(const Shuffle &S, const Input &I, unsigned Control,
             unsigned Destination = 0, unsigned Source = 1,
             bool Memory = false) {
    SCOPED_TRACE(S.Name);
    SCOPED_TRACE(Control);
    SCOPED_TRACE(Destination);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Memory);
    initialize(I, Destination, Source);
    auto Before = snapshot();
    const auto RAM = backing();
    const Input ActualInput{I.Left, !Memory && Destination == Source ? I.Left
                                                                     : I.Right};
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t Address, unsigned Size) {
      EXPECT_EQ(Address, MemoryAddress);
      EXPECT_EQ(Size, VectorBytes);
      EXPECT_EQ(snapshot(), Before);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(S, Control, Destination, Source, Memory);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Before[vectorRegister(GuestArchitecture::X64, Destination)] =
        scalarResult(S.Kind, ActualInput, Control);
    Before[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(Memory));
    EXPECT_EQ(Writes, 0u);
  }
  void controls(Operation Op) {
    const auto &S = Shuffles[unsigned(Op)];
    for (const auto &I : Inputs)
      for (unsigned Control = 0;
           Control <= (S.HasControl ? unsigned(UINT8_MAX) : 0u); ++Control) {
        check(S, I, Control);
        ASSERT_FALSE(HasFatalFailure());
        check(S, I, Control, 0, 0);
        ASSERT_FALSE(HasFatalFailure());
        check(S, I, Control, 0, 1, true);
        ASSERT_FALSE(HasFatalFailure());
      }
  }
};

#define NEVERD_SHUFFLE_OPERATION(Name, ...)                                    \
  TEST_P(X64Shuffle, Name##AllControlsPreserveCompleteState) {                 \
    controls(Operation::Name);                                                 \
  }
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_OPERATION

#define NEVERD_SHUFFLE_INTERLEAVE(Name, ...)                                   \
  TEST_P(X64Shuffle, Name##InterleavesRawLanes) { controls(Operation::Name); }
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INTERLEAVE

TEST_P(X64Shuffle, AllRegisterPairsRetainOriginalSources) {
  for (const auto &S : Shuffles)
    for (unsigned Destination = 0; Destination < XmmCount; ++Destination)
      for (unsigned Source = 0; Source < XmmCount; ++Source) {
        check(S, Inputs[(Destination + Source) % std::size(Inputs)],
              Destination * XmmCount + Source, Destination, Source);
        ASSERT_FALSE(HasFatalFailure());
      }
}

TEST_P(X64Shuffle, MemorySourcesCoverEveryDestination) {
  for (const auto &S : Shuffles)
    for (unsigned Destination = 0; Destination < XmmCount; ++Destination)
      for (unsigned Control : {0u, unsigned(UINT8_MAX)}) {
        check(S, Inputs[Destination % std::size(Inputs)], Control, Destination,
              1, true);
        ASSERT_FALSE(HasFatalFailure());
      }
}

TEST_P(X64Shuffle, MemoryStopsFaultsAndRetriesPreserveState) {
  enum class Action { Stop, Throw, Deny, Unmap, SupervisorOnly };
  for (const auto &S : Shuffles)
    for (unsigned Control : {0u, unsigned(UINT8_MAX)})
      for (auto A : {Action::Stop, Action::Throw, Action::Deny, Action::Unmap,
                     Action::SupervisorOnly}) {
        if (A == Action::SupervisorOnly && !GetParam().User)
          continue;
        SCOPED_TRACE(S.Name);
        SCOPED_TRACE(Control);
        SCOPED_TRACE(unsigned(A));
        reset();
        ASSERT_FALSE(HasFatalFailure());
        initialize(Inputs[0], 0, 1, Data);
        const auto Before = snapshot();
        const auto RAM = backing();
        if (A == Action::Deny || A == Action::SupervisorOnly)
          llvm::cantFail(CPU->protect(
              Data, PageSize, A == Action::Deny ? UserAccessible : Read));
        if (A == Action::Unmap)
          llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t Address, unsigned Size) {
          EXPECT_EQ(Address, Data);
          EXPECT_EQ(Size, VectorBytes);
          EXPECT_EQ(snapshot(), Before);
          ++Reads;
          if (A == Action::Stop)
            CPU->stop();
          if (A == Action::Throw)
            throw std::runtime_error(ObserverError);
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        Hooks.RecoverableFault = [](const BackendFault &) { return true; };
        const auto Bytes = instruction(S, Control, 0, 1, true);
        const auto Exit = run(Bytes, std::move(Hooks));
        const bool Fault = A != Action::Stop && A != Action::Throw;
        EXPECT_EQ(Exit.Kind, Fault ? ExecutionExitKind::RecoverableFault
                             : A == Action::Stop
                                 ? ExecutionExitKind::Stopped
                                 : ExecutionExitKind::BackendFailure)
            << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 0u);
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(backing(), RAM);
        if (A == Action::Throw)
          continue;
        if (Fault) {
          ASSERT_TRUE(Exit.Fault);
          EXPECT_EQ(Exit.Fault->Address, Data);
          EXPECT_EQ(Exit.Fault->Size, VectorBytes);
          EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
          EXPECT_EQ(Exit.Fault->Kind, A == Action::Unmap
                                          ? BackendFaultKind::UnmappedMemory
                                          : BackendFaultKind::Protection);
          ASSERT_TRUE(CPU->takeRecoverableFault());
          EXPECT_FALSE(CPU->takeRecoverableFault());
          if (A == Action::Unmap)
            llvm::cantFail(CPU->mapAlias(Data, Alias, PageSize,
                                         Read | Write | UserAccessible));
          else
            llvm::cantFail(
                CPU->protect(Data, PageSize, Read | Write | UserAccessible));
        }
        const auto Retried = run(Bytes);
        ASSERT_EQ(Retried.Kind, ExecutionExitKind::Stopped)
            << Retried.Diagnostic;
        auto Expected = Before;
        Expected[CPURegister::X64V0] = scalarResult(S.Kind, Inputs[0], Control);
        Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
        EXPECT_EQ(snapshot(), Expected);
        EXPECT_EQ(backing(), RAM);
      }
}

TEST_P(X64Shuffle, AlignmentFaultPrecedesMemoryAndCanRetry) {
  enum class Mapping { Readable, Denied, Absent };
  for (const auto &S : Shuffles)
    for (auto M : {Mapping::Readable, Mapping::Denied, Mapping::Absent}) {
      SCOPED_TRACE(S.Name);
      SCOPED_TRACE(unsigned(M));
      reset();
      ASSERT_FALSE(HasFatalFailure());
      initialize(Inputs[1], 0, 1, Data + 1);
      auto Before = snapshot();
      const auto RAM = backing();
      if (M == Mapping::Denied)
        llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
      if (M == Mapping::Absent)
        llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
      unsigned Observations = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
      Hooks.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Bytes = instruction(S, UINT8_MAX, 0, 1, true);
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Interrupt);
      EXPECT_EQ(Exit.Fault->Interrupt,
                unsigned(x64::ExceptionVector::GeneralProtection));
      EXPECT_EQ(Exit.Fault->ErrorCode, 0u);
      EXPECT_EQ(Exit.Fault->Cause, BackendFaultCause::OperandAlignment);
      EXPECT_EQ(Observations, 0u);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(backing(), RAM);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      llvm::cantFail(CPU->setReg(X64Register::CX, MemoryAddress));
      const auto Retried = run(Bytes);
      ASSERT_EQ(Retried.Kind, ExecutionExitKind::Stopped) << Retried.Diagnostic;
      Before[CPURegister::X64CX] = {MemoryAddress, 0};
      Before[CPURegister::X64V0] = scalarResult(S.Kind, Inputs[1], UINT8_MAX);
      Before[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64Shuffle, InstructionStopsAndFailuresPreserveState) {
  for (const auto &S : Shuffles)
    for (bool Fail : {false, true}) {
      reset();
      ASSERT_FALSE(HasFatalFailure());
      initialize(Inputs[0]);
      auto Before = snapshot();
      const auto RAM = backing();
      const auto Bytes = instruction(S, UINT8_MAX, 0, 1, true);
      llvm::cantFail(CPU->write(Code, Bytes));
      unsigned Instructions = 0, Accesses = 0;
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t PC, unsigned Size) {
        EXPECT_EQ(PC, Code);
        EXPECT_EQ(Size, Bytes.size());
        EXPECT_EQ(snapshot(), Before);
        ++Instructions;
        CPU->stop();
        if (Fail)
          throw std::runtime_error(ObserverError);
      };
      Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
      llvm::cantFail(CPU->installHooks(std::move(Hooks)));
      const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
      EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_TRUE(Exit.StopRequested);
      EXPECT_EQ(Instructions, 1u);
      EXPECT_EQ(Accesses, 0u);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64Shuffle, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    reset();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Inputs[0]);
    const auto Before = snapshot();
    const auto RAM = backing();
    unsigned Observations = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Exit = run(Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(backing(), RAM);
  };
  for (const auto &S : Shuffles)
    for (bool Memory : {false, true}) {
      auto Bytes = instruction(S, UINT8_MAX, 0, 1, Memory);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes);
      ASSERT_FALSE(HasFatalFailure());
    }
#define NEVERD_SHUFFLE_INVALID(Name, ...)                                      \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64ShuffleCases.def"
#undef NEVERD_SHUFFLE_INVALID
}

TEST_P(X64Shuffle, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &S : Shuffles) {
    reset();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Inputs[0], 0, 1, Stack);
    const auto Before = snapshot();
    const auto RAM = backing();
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
    Device.PrepareRead =
        [&](uint64_t, unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Calls;
      return GuestMMIOPreparedRead{0, [&] {
                                     ++Calls;
                                     return llvm::Error::success();
                                   }};
    };
    auto Mapped = CPU->mapMMIO(Stack, PageSize, std::move(Device));
    if (GetParam().User)
      EXPECT_EQ(llvm::toString(std::move(Mapped)), diagnostic::DeviceMapping);
    else {
      ASSERT_EQ(llvm::toString(std::move(Mapped)), "");
      const auto Exit = run(instruction(S, 0, 0, 1, true));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
    }
    EXPECT_EQ(Calls, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(backing(), RAM);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Shuffle,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

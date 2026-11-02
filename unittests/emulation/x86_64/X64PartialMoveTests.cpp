//===- X64PartialMoveTests.cpp - Exact SSE half-register transfers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/Support/Compiler.h"

#include <stdexcept>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_PARTIAL_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PARTIAL_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_PARTIAL_INVALID(Name, ...)                                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_INVALID
#undef NEVERD_PARTIAL_TEXT
#undef NEVERD_PARTIAL_VALUE
enum class Operation {
#define NEVERD_PARTIAL_MOVE(Name, ...) Name,
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_MOVE
};
struct Move {
  const char *Name;
  Operation Kind;
  uint8_t Prefix, Load, Store, Lane;
};
constexpr Move Moves[] = {
#define NEVERD_PARTIAL_MOVE(Name, Prefix, Load, Store, Lane)                   \
  {#Name, Operation::Name, Prefix, Load, Store, Lane},
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_MOVE
};
constexpr Input Inputs[] = {
#define NEVERD_PARTIAL_INPUT(Low, High, SourceLow, SourceHigh)                 \
  {{Low, High}, {SourceLow, SourceHigh}},
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_INPUT
};
constexpr uint64_t Offsets[] = {
#define NEVERD_PARTIAL_OFFSET(Value) Value,
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_OFFSET
};

#if defined(__x86_64__) || defined(_M_X64)
LLVM_ATTRIBUTE_NOINLINE RegisterValue nativeLoad(Operation Op, const Input &I) {
  const auto V =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Left.data()));
  __m128i Result{};
  switch (Op) {
  case Operation::MOVLPS:
    Result = _mm_castps_si128(_mm_loadl_pi(
        _mm_castsi128_ps(V), reinterpret_cast<const __m64 *>(I.Right.data())));
    break;
  case Operation::MOVHPS:
    Result = _mm_castps_si128(_mm_loadh_pi(
        _mm_castsi128_ps(V), reinterpret_cast<const __m64 *>(I.Right.data())));
    break;
  case Operation::MOVLPD:
    Result = _mm_castpd_si128(_mm_loadl_pd(
        _mm_castsi128_pd(V), reinterpret_cast<const double *>(I.Right.data())));
    break;
  case Operation::MOVHPD:
    Result = _mm_castpd_si128(_mm_loadh_pd(
        _mm_castsi128_pd(V), reinterpret_cast<const double *>(I.Right.data())));
    break;
  }
  RegisterValue Actual;
  _mm_storeu_si128(reinterpret_cast<__m128i *>(Actual.data()), Result);
  return Actual;
}
LLVM_ATTRIBUTE_NOINLINE RegisterValue nativeStore(Operation Op,
                                                  const Input &I) {
  const auto V =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(I.Left.data()));
  auto Result = I.Right;
  switch (Op) {
  case Operation::MOVLPS:
    _mm_storel_pi(reinterpret_cast<__m64 *>(Result.data()),
                  _mm_castsi128_ps(V));
    break;
  case Operation::MOVHPS:
    _mm_storeh_pi(reinterpret_cast<__m64 *>(Result.data()),
                  _mm_castsi128_ps(V));
    break;
  case Operation::MOVLPD:
    _mm_storel_pd(reinterpret_cast<double *>(Result.data()),
                  _mm_castsi128_pd(V));
    break;
  case Operation::MOVHPD:
    _mm_storeh_pd(reinterpret_cast<double *>(Result.data()),
                  _mm_castsi128_pd(V));
    break;
  }
  return Result;
}
#endif

TEST(X64PartialMoveOracle, ScalarHalvesMatchNativeLoadsAndStores) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &M : Moves)
    for (const auto &I : Inputs) {
      SCOPED_TRACE(M.Name);
      auto Expected = I.Left;
      Expected[M.Lane] = I.Right[0];
      EXPECT_EQ(nativeLoad(M.Kind, I), Expected);
      EXPECT_EQ(nativeStore(M.Kind, I),
                (RegisterValue{I.Left[M.Lane], I.Right[1]}));
    }
#else
  GTEST_SKIP();
#endif
}

std::vector<uint8_t> instruction(const Move &M, bool Store, unsigned Index) {
  std::vector<uint8_t> Bytes;
  if (M.Prefix)
    Bytes.push_back(M.Prefix);
  if (Index > RegisterMask)
    Bytes.push_back(Rex | RexVector);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(Store ? M.Store : M.Load);
  Bytes.push_back(((Index & RegisterMask) << RegisterShift) | MemoryModRM);
  return Bytes;
}

class X64PartialMove : public X64VectorTest {
protected:
  void SetUp() override { resetMemory(); }
  void resetMemory() {
    reset();
    if (!CPU)
      return;
    // Each page has an independent physical owner; aliases retain faulted RAM.
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->mapAlias(Alias + PageSize, Data + PageSize, PageSize,
                                 Read | Write | UserAccessible));
  }
  void initialize(const Input &I, unsigned Index, uint64_t Address) {
    seed(I, Address);
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setXmm(Index, I.Left));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FP0,
                                      {SentinelLow, PhysicalExponent}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FPTag, {1, 0}));
    std::array<uint8_t, 2 * PageSize> RAM{};
    for (size_t N = 0; N < RAM.size(); ++N)
      RAM[N] = N * PatternStride + N / PageSize;
    llvm::cantFail(CPU->write(Alias, RAM));
    if (Address >= Data && Address < Data + RAM.size())
      llvm::cantFail(CPU->writeInteger(Address, I.Right[0], WordBytes));
    else if (Address >= Alias && Address < Alias + RAM.size())
      llvm::cantFail(CPU->writeInteger(Address, I.Right[0], WordBytes));
  }
  auto backing(bool Observing = false) {
    std::array<uint8_t, 2 * PageSize> Bytes{};
    // Diagnostic snapshots require a stopped CPU. Observers read the live,
    // readable alias before effects; check errors in Release builds as well.
    EXPECT_EQ(llvm::toString(Observing ? CPU->read(Alias, Bytes)
                                       : CPU->snapshotBacking(Alias, Bytes)),
              "");
    return Bytes;
  }
  void check(const Move &M, bool Store, unsigned Index, const Input &I,
             uint64_t Offset) {
    SCOPED_TRACE(M.Name);
    SCOPED_TRACE(Store);
    SCOPED_TRACE(Index);
    SCOPED_TRACE(Offset);
    initialize(I, Index, Alias + Offset);
    auto Expected = snapshot();
    auto RAM = backing();
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t Address, unsigned Size) {
      EXPECT_EQ(Address, Alias + Offset);
      EXPECT_EQ(Size, WordBytes);
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t Address, unsigned Size, uint64_t Value) {
      EXPECT_EQ(Address, Alias + Offset);
      EXPECT_EQ(Size, WordBytes);
      EXPECT_EQ(Value, I.Left[M.Lane]);
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Writes;
    };
    const auto Bytes = instruction(M, Store, Index);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    if (Store)
      llvm::support::endian::write64le(RAM.data() + Offset, I.Left[M.Lane]);
    else
      Expected[vectorRegister(GuestArchitecture::X64, Index)][M.Lane] =
          I.Right[0];
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(!Store));
    EXPECT_EQ(Writes, unsigned(Store));
  }
};

#define NEVERD_PARTIAL_MOVE(Name, ...)                                         \
  TEST_P(X64PartialMove, Name##LoadsAndStoresPreserveOtherHalf) {              \
    for (unsigned Index = 0; Index < XmmCount; ++Index)                        \
      for (const auto &I : Inputs)                                             \
        for (auto Offset : Offsets)                                            \
          for (bool Store : {false, true}) {                                   \
            check(Moves[unsigned(Operation::Name)], Store, Index, I, Offset);  \
            ASSERT_FALSE(HasFatalFailure());                                   \
          }                                                                    \
  }
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_MOVE

TEST_P(X64PartialMove, CrossPageFaultsPreserveBothPagesAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &M : Moves)
    for (bool Store : {false, true})
      for (unsigned Page : {0u, 1u})
        for (auto Kind :
             {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
          if (Kind == Mapping::SupervisorOnly && !GetParam().User)
            continue;
          SCOPED_TRACE(M.Name);
          SCOPED_TRACE(Store);
          SCOPED_TRACE(Page);
          SCOPED_TRACE(unsigned(Kind));
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          const uint64_t Address = Data + PageSize - BoundaryTail;
          initialize(Inputs[0], XmmCount - 1, Address);
          auto Expected = snapshot();
          auto RAM = backing();
          const uint64_t PageAddress = Data + Page * PageSize;
          if (Kind == Mapping::Absent)
            llvm::cantFail(CPU->addressSpace()->unmap(PageAddress, PageSize));
          else
            llvm::cantFail(
                CPU->protect(PageAddress, PageSize,
                             Kind == Mapping::SupervisorOnly
                                 ? Read | Write
                                 : (Store ? Read : Write) | UserAccessible));
          unsigned Reads = 0, Writes = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t A, unsigned Size) {
            EXPECT_EQ(A, Address);
            EXPECT_EQ(Size, WordBytes);
            ++Reads;
          };
          Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
            EXPECT_EQ(A, Address);
            EXPECT_EQ(Size, WordBytes);
            EXPECT_EQ(Value, Inputs[0].Left[M.Lane]);
            ++Writes;
          };
          Hooks.RecoverableFault = [](const BackendFault &) { return true; };
          const auto Bytes = instruction(M, Store, XmmCount - 1);
          const auto Exit = run(Bytes, std::move(Hooks));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
              << Exit.Diagnostic;
          ASSERT_TRUE(Exit.Fault);
          EXPECT_EQ(Exit.Fault->Address, Page ? PageAddress : Address);
          EXPECT_EQ(Exit.Fault->Size, BoundaryTail);
          EXPECT_EQ(Exit.Fault->Access,
                    Store ? BackendAccessKind::Write : BackendAccessKind::Read);
          EXPECT_EQ(Exit.Fault->Kind, Kind == Mapping::Absent
                                          ? BackendFaultKind::UnmappedMemory
                                          : BackendFaultKind::Protection);
          EXPECT_EQ(Reads, unsigned(!Store));
          EXPECT_EQ(Writes, unsigned(Store));
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), RAM);
          ASSERT_TRUE(CPU->takeRecoverableFault());
          EXPECT_FALSE(CPU->takeRecoverableFault());
          if (Kind == Mapping::Absent)
            llvm::cantFail(CPU->mapAlias(PageAddress, Alias + Page * PageSize,
                                         PageSize,
                                         Read | Write | UserAccessible));
          else
            llvm::cantFail(CPU->protect(PageAddress, PageSize,
                                        Read | Write | UserAccessible));
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          if (Store)
            llvm::support::endian::write64le(
                RAM.data() + PageSize - BoundaryTail, Inputs[0].Left[M.Lane]);
          else
            Expected[vectorRegister(GuestArchitecture::X64, XmmCount - 1)]
                    [M.Lane] = Inputs[0].Right[0];
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), RAM);
        }
}

TEST_P(X64PartialMove, ObserverStopsAndFailuresPreserveState) {
  for (const auto &M : Moves)
    for (bool Store : {false, true})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        const uint64_t Address = Alias + PageSize - BoundaryTail;
        initialize(Inputs[2], XmmCount - 1, Address);
        const auto Before = snapshot();
        const auto RAM = backing();
        unsigned Reads = 0, Writes = 0;
        auto Observe = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, WordBytes);
          EXPECT_EQ(snapshot(), Before);
          EXPECT_EQ(backing(true), RAM);
          if (Fail)
            throw std::runtime_error(ObserverError);
          CPU->stop();
        };
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          ++Reads;
          Observe(A, Size);
        };
        Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
          ++Writes;
          EXPECT_EQ(Value, Inputs[2].Left[M.Lane]);
          Observe(A, Size);
        };
        const auto Bytes = instruction(M, Store, XmmCount - 1);
        const auto Exit = run(Bytes, std::move(Hooks));
        EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                  : ExecutionExitKind::Stopped)
            << Exit.Diagnostic;
        EXPECT_EQ(Reads, unsigned(!Store));
        EXPECT_EQ(Writes, unsigned(Store));
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(backing(), RAM);
        if (!Fail) {
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          auto Expected = Before;
          auto Updated = RAM;
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          if (Store)
            llvm::support::endian::write64le(Updated.data() + PageSize -
                                                 BoundaryTail,
                                             Inputs[2].Left[M.Lane]);
          else
            Expected[vectorRegister(GuestArchitecture::X64, XmmCount - 1)]
                    [M.Lane] = Inputs[2].Right[0];
          EXPECT_EQ(snapshot(), Expected);
          EXPECT_EQ(backing(), Updated);
        }
      }
}

TEST_P(X64PartialMove, PageEndOperandsDoNotAccessTheNextPage) {
  for (const auto &M : Moves)
    for (bool Store : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      initialize(Inputs[0], 0, Data + PageSize - WordBytes);
      auto Expected = snapshot();
      auto RAM = backing();
      llvm::cantFail(CPU->addressSpace()->unmap(Data + PageSize, PageSize));
      const auto Bytes = instruction(M, Store, 0);
      const auto Exit = run(Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      if (Store)
        llvm::support::endian::write64le(RAM.data() + PageSize - WordBytes,
                                         Inputs[0].Left[M.Lane]);
      else
        Expected[CPURegister::X64V0][M.Lane] = Inputs[0].Right[0];
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64PartialMove, StoresDoNotRequireReadPermission) {
  for (const auto &M : Moves) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Inputs[0], 0, Data + 1);
    auto Expected = snapshot();
    auto RAM = backing();
    llvm::cantFail(CPU->protect(Data, PageSize, Write | UserAccessible));
    unsigned Reads = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
    const auto Bytes = instruction(M, true, 0);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    llvm::support::endian::write64le(RAM.data() + 1, Inputs[0].Left[M.Lane]);
    EXPECT_EQ(snapshot(), Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, 0u);
  }
}

TEST_P(X64PartialMove, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Inputs[0], 0, Data);
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
  for (const auto &M : Moves)
    for (bool Store : {false, true}) {
      auto Bytes = instruction(M, Store, 0);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes);
      ASSERT_FALSE(HasFatalFailure());
    }
#define NEVERD_PARTIAL_INVALID(Name, ...)                                      \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64PartialMoveCases.def"
#undef NEVERD_PARTIAL_INVALID
}

TEST_P(X64PartialMove, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &M : Moves)
    for (bool Store : {false, true}) {
      resetMemory();
      ASSERT_FALSE(HasFatalFailure());
      initialize(Inputs[0], 0, Stack);
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
        const auto Exit = run(instruction(M, Store, 0));
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
            << Exit.Diagnostic;
      }
      EXPECT_EQ(Calls, 0u);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(backing(), RAM);
    }
}

TEST_P(X64PartialMove, RegisterEncodingsRetainDistinctLaneSemantics) {
  for (const auto &M : Moves) {
    if (M.Prefix)
      continue;
    for (unsigned Destination = 0; Destination < XmmCount; ++Destination)
      for (unsigned Source = 0; Source < XmmCount; ++Source) {
        initialize(Inputs[0], Destination, Data);
        if (Destination != Source)
          llvm::cantFail(CPU->setXmm(Source, Inputs[0].Right));
        auto Expected = snapshot();
        const auto RAM = backing();
        const auto Original =
            Expected[vectorRegister(GuestArchitecture::X64, Source)];
        std::vector<uint8_t> Bytes;
        const auto Extension = (Destination > RegisterMask ? RexVector : 0) |
                               (Source > RegisterMask ? RexSource : 0);
        if (Extension)
          Bytes.push_back(Rex | Extension);
        Bytes.push_back(OpcodeMap);
        Bytes.push_back(M.Load);
        Bytes.push_back(RegisterModRM |
                        ((Destination & RegisterMask) << RegisterShift) |
                        (Source & RegisterMask));
        unsigned Accesses = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
        const auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        Expected[vectorRegister(GuestArchitecture::X64, Destination)][M.Lane] =
            Original[1 - M.Lane];
        Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
        EXPECT_EQ(snapshot(), Expected);
        EXPECT_EQ(backing(), RAM);
        EXPECT_EQ(Accesses, 0u);
      }
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PartialMove,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

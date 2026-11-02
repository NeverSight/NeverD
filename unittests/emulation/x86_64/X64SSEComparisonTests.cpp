//===- X64SSEComparisonTests.cpp - Scalar SSE flags and exceptions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64DAZTestSupport.h"
#include "arch/x86_64/X64Machine.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <climits>
#include <cstring>
#include <stdexcept>

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_SSE_COMPARE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SSE_COMPARE_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_SSE_COMPARE_INVALID(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INVALID
#undef NEVERD_SSE_COMPARE_TEXT
#undef NEVERD_SSE_COMPARE_VALUE
struct Comparison {
  const char *Name;
  uint8_t Prefix, Opcode;
  unsigned Bits;
  bool SignalsQuietNaN;
};
constexpr Comparison Comparisons[] = {
#define NEVERD_SSE_COMPARE_OPERATION(Name, Prefix, Opcode, Bits, Quiet)        \
  {#Name, Prefix, Opcode, Bits, Quiet},
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_OPERATION
};
constexpr uint64_t Roundings[] = {
#define NEVERD_SSE_COMPARE_ROUND(Name, Bits) Bits,
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_ROUND
};
struct NumericInput {
  const char *Name;
  uint64_t Single, Double;
};
#define NEVERD_SSE_COMPARE_INPUT(Name, Single, Double)                         \
  constexpr NumericInput Name = {#Name, Single, Double};
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INPUT
constexpr NumericInput Inputs[] = {
#define NEVERD_SSE_COMPARE_INPUT(Name, Single, Double) Name,
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INPUT
};
unsigned sourceBytes(const Comparison &C) { return C.Bits / CHAR_BIT; }
Input input(const Comparison &C, const NumericInput &Left,
            const NumericInput &Right) {
  auto Vector = [&](const NumericInput &Value, uint64_t Low, uint64_t High) {
    return RegisterValue{C.Bits == SingleBits
                             ? (Low & ~uint64_t(UINT32_MAX)) | Value.Single
                             : Value.Double,
                         High};
  };
  return {Vector(Left, SentinelLow, SentinelHigh),
          Vector(Right, SentinelHigh, SentinelLow)};
}
struct Result {
  uint64_t Flags, MXCSR;
};
Result reference(const Comparison &C, const Input &I, uint64_t InitialFlags,
                 uint64_t Control) {
  const auto &Format = C.Bits == SingleBits ? llvm::APFloat::IEEEsingle()
                                            : llvm::APFloat::IEEEdouble();
  const auto Left =
      daz_test::operand(Format, llvm::APInt(C.Bits, I.Left[0]), Control);
  const auto Right =
      daz_test::operand(Format, llvm::APInt(C.Bits, I.Right[0]), Control);
  uint64_t Condition = 0, Status = 0;
  switch (Left.compare(Right)) {
  case llvm::APFloat::cmpEqual:
    Condition = EqualFlags;
    break;
  case llvm::APFloat::cmpLessThan:
    Condition = LessFlags;
    break;
  case llvm::APFloat::cmpGreaterThan:
    break;
  case llvm::APFloat::cmpUnordered:
    Condition = UnorderedFlags;
    break;
  }
  // NaN handling has priority over a denormal in the other operand, including
  // a quiet NaN that UCOMIS does not report as an invalid operation.
  if (Left.isNaN() || Right.isNaN()) {
    if (C.SignalsQuietNaN || Left.isSignaling() || Right.isSignaling())
      Status = InvalidStatus;
  } else if (Left.isDenormal() || Right.isDenormal())
    Status = DenormalStatus;
  return {(InitialFlags & ~ArithmeticFlags) | Condition, Control | Status};
}
std::vector<uint8_t> instruction(const Comparison &C, unsigned Dest = 0,
                                 unsigned Source = 1, bool Memory = false) {
  std::vector<uint8_t> Bytes;
  if (C.Prefix)
    Bytes.push_back(C.Prefix);
  const uint8_t Extension = (Dest > RegisterMask ? RexDestination : 0) |
                            (!Memory && Source > RegisterMask ? RexSource : 0);
  if (Extension)
    Bytes.push_back(Rex | Extension);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(C.Opcode);
  Bytes.push_back(
      ((Dest & RegisterMask) << RegisterShift) |
      (Memory ? MemoryModRM : RegisterModRM | (Source & RegisterMask)));
  return Bytes;
}

void nativeOracle(uint64_t DAZ) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Mask = daz_test::hostMXCSRMask();
  if (DAZ && !(Mask & DAZ))
    GTEST_SKIP() << daz_test::HostUnavailable;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
#define NEVERD_SSE_COMPARE_BYTES(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_BYTES
  for (const auto &C : Comparisons)
    for (bool Memory : {false, true}) {
#ifdef _WIN32
      std::vector<uint8_t> Bytes(std::begin(Win64Before),
                                 std::end(Win64Before));
      const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
      std::vector<uint8_t> Bytes(std::begin(SysVBefore), std::end(SysVBefore));
      const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
      Bytes.insert(Bytes.end(), std::begin(BeforeComparison),
                   std::end(BeforeComparison));
      const auto Compare = instruction(C, 0, 1, Memory);
      Bytes.insert(Bytes.end(), Compare.begin(), Compare.end());
      Bytes.insert(Bytes.end(), std::begin(AfterComparison),
                   std::end(AfterComparison));
      Bytes.insert(Bytes.end(), After.begin(), After.end());
      std::error_code EC;
      auto Block = llvm::sys::Memory::allocateMappedMemory(
          PageSize, nullptr,
          llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      auto Release = llvm::scope_exit(
          [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
      std::memcpy(Block.base(), Bytes.data(), Bytes.size());
      EC = llvm::sys::Memory::protectMappedMemory(
          Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
      auto Execute =
          reinterpret_cast<uint64_t (*)(void *, void *, void *, const void *)>(
              Block.base());
      for (const auto &Left : Inputs)
        for (const auto &Right : Inputs)
          for (auto Rounding : Roundings)
            for (auto InitialFlags : {ClearFlags, Flags, Flags | DirectionFlag})
              for (auto Sticky :
                   {uint64_t(0), ExistingStatus,
                    ExistingStatus | InvalidStatus | DenormalStatus})
                for (auto Flush : {uint64_t(0), FlushToZero}) {
                  SCOPED_TRACE(C.Name);
                  SCOPED_TRACE(Memory);
                  SCOPED_TRACE(Left.Name);
                  SCOPED_TRACE(Right.Name);
                  SCOPED_TRACE(InitialFlags);
                  SCOPED_TRACE(Rounding);
                  const auto I = input(C, Left, Right);
                  X64MachineState Seed;
                  for (unsigned N = 0; N < XmmCount; ++N)
                    Seed.Xmm[N] = {SentinelLow + N, SentinelHigh - N};
                  Seed.Xmm[0] = I.Left;
                  Seed.Xmm[1] = I.Right;
                  Seed.MXCSR = InitialMXCSR | DAZ | Rounding | Sticky | Flush;
                  alignas(x64::fp::RegisterSlotBytes)
                      std::array<uint8_t, x64::fp::LegacyBytes>
                          Before{}, After{}, Host{};
                  ASSERT_EQ(
                      llvm::toString(encodeX64FXState(Seed, Before, Mask)), "");
                  const std::array<uint64_t, 2> Arguments{I.Right[0],
                                                          InitialFlags};
                  const auto ActualFlags =
                      Execute(Before.data(), After.data(), Host.data(),
                              Arguments.data());
                  X64MachineState Actual;
                  ASSERT_EQ(llvm::toString(decodeX64FXState(Actual, After)),
                            "");
                  const auto Expected =
                      reference(C, I, InitialFlags, Seed.MXCSR);
                  EXPECT_EQ(ActualFlags, Expected.Flags);
                  EXPECT_EQ(Actual.Xmm, Seed.Xmm);
                  EXPECT_EQ(Actual.MXCSR, Expected.MXCSR);
                }
    }
#else
  GTEST_SKIP();
#endif
}

TEST(X64SSEComparisonOracle,
     FlagsAndExceptionPriorityMatchOriginalInstructions) {
  nativeOracle(0);
}
TEST(X64SSEComparisonDAZOracle,
     FlagsAndExceptionPriorityMatchOriginalInstructions) {
  nativeOracle(daz_test::DenormalsAreZero);
}

class X64SSEComparison : public X64VectorTest {
protected:
  void SetUp() override { resetMemory(); }
  void resetMemory() {
    reset();
    if (!CPU)
      return;
    ASSERT_EQ(llvm::toString(CPU->map(Data + PageSize, PageSize,
                                      Read | Write | UserAccessible)),
              "");
    ASSERT_EQ(
        llvm::toString(CPU->mapAlias(Alias + PageSize, Data + PageSize,
                                     PageSize, Read | Write | UserAccessible)),
        "");
  }
  void initialize(const Comparison &C, const Input &I, uint64_t Control,
                  unsigned Dest = 0, unsigned Source = 1, bool Memory = false,
                  uint64_t Address = Data, uint64_t InitialFlags = Flags) {
    seed(I, Address);
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::PC, Code)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::MXCSR, Control)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::FLAGS, InitialFlags)),
              "");
    ASSERT_EQ(llvm::toString(CPU->setXmm(Dest, I.Left)), "");
    if (!Memory)
      ASSERT_EQ(llvm::toString(CPU->setXmm(Source, I.Right)), "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(
                  CPURegister::X64FP0, {SentinelLow, PhysicalExponent})),
              "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(CPURegister::X64FPTag, {1, 0})),
              "");
    std::array<uint8_t, 2 * PageSize> RAM{};
    for (size_t N = 0; N < RAM.size(); ++N)
      RAM[N] = N * PatternStride + N / PageSize;
    ASSERT_EQ(llvm::toString(CPU->write(Alias, RAM)), "");
    if (Memory && Address != Stack)
      ASSERT_EQ(llvm::toString(
                    CPU->writeInteger(Address, I.Right[0], sourceBytes(C))),
                "");
  }
  auto backing(bool Observing = false) {
    std::array<uint8_t, 2 * PageSize> RAM{};
    EXPECT_EQ(llvm::toString(Observing ? CPU->read(Alias, RAM)
                                       : CPU->snapshotBacking(Alias, RAM)),
              "");
    return RAM;
  }
  void expectSnapshot(const std::map<CPURegister, RegisterValue> &Expected) {
    const auto Actual = snapshot();
    ASSERT_EQ(Actual.size(), Expected.size());
    for (const auto &[R, Value] : Expected) {
      SCOPED_TRACE(static_cast<unsigned>(R));
      ASSERT_NE(Actual.find(R), Actual.end());
      EXPECT_EQ(Actual.at(R), Value);
    }
  }
  void check(const Comparison &C, Input I, uint64_t Control, unsigned Dest = 0,
             unsigned Source = 1, bool Memory = false, uint64_t Address = Data,
             uint64_t InitialFlags = Flags) {
    SCOPED_TRACE(C.Name);
    SCOPED_TRACE(Dest);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Memory);
    SCOPED_TRACE(Control);
    SCOPED_TRACE(InitialFlags);
    if (!Memory && Dest == Source)
      I.Right = I.Left;
    initialize(C, I, Control, Dest, Source, Memory, Address, InitialFlags);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    const auto Value = reference(C, I, InitialFlags, Control);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, sourceBytes(C));
      expectSnapshot(Expected);
      EXPECT_EQ(backing(true), RAM);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Bytes = instruction(C, Dest, Source, Memory);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Expected[CPURegister::X64FLAGS] = {Value.Flags, 0};
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    expectSnapshot(Expected);
    EXPECT_EQ(backing(), RAM);
    EXPECT_EQ(Reads, unsigned(Memory));
    EXPECT_EQ(Writes, 0u);
  }
  void compare(const Comparison &C, uint64_t DAZ = 0) {
    for (const auto &Left : Inputs)
      for (const auto &Right : Inputs)
        for (auto Sticky : {uint64_t(0), ExistingStatus,
                            ExistingStatus | InvalidStatus | DenormalStatus})
          for (auto Flush : {uint64_t(0), FlushToZero}) {
            SCOPED_TRACE(Left.Name);
            SCOPED_TRACE(Right.Name);
            const auto I = input(C, Left, Right);
            const auto Control = InitialMXCSR | DAZ | Sticky | Flush;
            check(C, I, Control);
            ASSERT_FALSE(HasFatalFailure());
            check(C, I, Control, 0, 1, true, Alias + 1);
            ASSERT_FALSE(HasFatalFailure());
            check(C, I, Control, 0, 1, true,
                  Alias + PageSize - sourceBytes(C) / 2);
            ASSERT_FALSE(HasFatalFailure());
          }
  }
  void rounding(uint64_t DAZ = 0) {
    const std::pair<NumericInput, NumericInput> Pairs[] = {
#define NEVERD_SSE_COMPARE_PAIR(Left, Right) {Left, Right},
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_PAIR
    };
    for (const auto &C : Comparisons)
      for (const auto &[Left, Right] : Pairs)
        for (auto Rounding : Roundings)
          for (auto InitialFlags : {ClearFlags, Flags, Flags | DirectionFlag})
            for (auto Sticky :
                 {uint64_t(0), ExistingStatus,
                  ExistingStatus | InvalidStatus | DenormalStatus})
              for (auto Flush : {uint64_t(0), FlushToZero})
                for (bool Memory : {false, true}) {
                  check(C, input(C, Left, Right),
                        InitialMXCSR | DAZ | Rounding | Sticky | Flush, 0, 1,
                        Memory, Data, InitialFlags);
                  ASSERT_FALSE(HasFatalFailure());
                }
  }
};
#define NEVERD_SSE_COMPARE_OPERATION(Name, Prefix, Opcode, Bits, Quiet)        \
  TEST_P(X64SSEComparison, Name##FlagsAndStatusPreserveSources) {              \
    compare({#Name, Prefix, Opcode, Bits, Quiet});                             \
  }
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_OPERATION

TEST_P(X64SSEComparison, RoundingModesAndUnrelatedFlagsRemainIndependent) {
  rounding();
}

TEST_P(X64SSEComparison, EveryVectorPairAndAliasedSource) {
  for (const auto &C : Comparisons)
    for (unsigned Dest = 0; Dest < XmmCount; ++Dest) {
      for (unsigned Source = 0; Source < XmmCount; ++Source) {
        check(C,
              input(C, Inputs[Dest % std::size(Inputs)],
                    Inputs[(Dest + Source) % std::size(Inputs)]),
              InitialMXCSR | ExistingStatus, Dest, Source);
        ASSERT_FALSE(HasFatalFailure());
      }
      // An aliased source is still unordered for NaNs and still reports a
      // denormal operand; identical register numbers do not imply equality.
      for (const auto &Value : {QuietNaN, SignalingNaN, MinSubnormal}) {
        check(C, input(C, Value, One), InitialMXCSR, Dest, Dest);
        ASSERT_FALSE(HasFatalFailure());
      }
      check(C, input(C, NegativeOne, MinSubnormal), InitialMXCSR, Dest, 1,
            true);
      ASSERT_FALSE(HasFatalFailure());
    }
}

TEST_P(X64SSEComparison, CrossPageFaultsPreserveStatusAndRetry) {
  enum class Mapping { Denied, Absent, SupervisorOnly };
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
      for (unsigned Page : {0u, 1u})
        for (auto Kind :
             {Mapping::Denied, Mapping::Absent, Mapping::SupervisorOnly}) {
          if (Kind == Mapping::SupervisorOnly && !GetParam().User)
            continue;
          resetMemory();
          ASSERT_FALSE(HasFatalFailure());
          const auto Width = sourceBytes(C);
          const uint64_t Address = Data + PageSize - Width / 2;
          const uint64_t Control = InitialMXCSR | ExistingStatus;
          initialize(C, input(C, One, Input), Control, XmmCount - 1, 0, true,
                     Address);
          ASSERT_FALSE(HasFatalFailure());
          auto Expected = snapshot();
          const auto RAM = backing();
          const auto PageAddress = Data + Page * PageSize;
          if (Kind == Mapping::Absent)
            ASSERT_EQ(llvm::toString(
                          CPU->addressSpace()->unmap(PageAddress, PageSize)),
                      "");
          else
            ASSERT_EQ(llvm::toString(CPU->protect(PageAddress, PageSize,
                                                  Kind == Mapping::Denied
                                                      ? Write | UserAccessible
                                                      : Read | Write)),
                      "");
          unsigned Reads = 0, Writes = 0;
          BackendHooks Hooks;
          Hooks.Read = [&](uint64_t A, unsigned Size) {
            EXPECT_EQ(A, Address);
            EXPECT_EQ(Size, Width);
            ++Reads;
          };
          Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
          Hooks.RecoverableFault = [](const BackendFault &) { return true; };
          const auto Bytes = instruction(C, XmmCount - 1, 0, true);
          const auto Exit = run(Bytes, std::move(Hooks));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
              << Exit.Diagnostic;
          ASSERT_TRUE(Exit.Fault);
          EXPECT_EQ(Exit.Fault->Address, Page ? PageAddress : Address);
          EXPECT_EQ(Exit.Fault->Size, Width / 2);
          EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
          EXPECT_EQ(Exit.Fault->Kind, Kind == Mapping::Absent
                                          ? BackendFaultKind::UnmappedMemory
                                          : BackendFaultKind::Protection);
          EXPECT_EQ(Reads, 1u);
          EXPECT_EQ(Writes, 0u);
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
          ASSERT_TRUE(CPU->takeRecoverableFault());
          EXPECT_FALSE(CPU->takeRecoverableFault());
          if (Kind == Mapping::Absent)
            ASSERT_EQ(llvm::toString(CPU->mapAlias(
                          PageAddress, Alias + Page * PageSize, PageSize,
                          Read | Write | UserAccessible)),
                      "");
          else
            ASSERT_EQ(
                llvm::toString(CPU->protect(PageAddress, PageSize,
                                            Read | Write | UserAccessible)),
                "");
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value = reference(C, input(C, One, Input), Flags, Control);
          Expected[CPURegister::X64FLAGS] = {Value.Flags, 0};
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
}

TEST_P(X64SSEComparison, ReadObserversStopBeforeComparisonAndStatus) {
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(C, input(C, One, Input), InitialMXCSR, 0, 0, true, Data + 1);
        ASSERT_FALSE(HasFatalFailure());
        auto Expected = snapshot();
        const auto RAM = backing();
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Data + 1);
          EXPECT_EQ(Size, sourceBytes(C));
          expectSnapshot(Expected);
          EXPECT_EQ(backing(true), RAM);
          ++Reads;
          if (Fail)
            throw std::runtime_error(ObserverError);
          CPU->stop();
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        const auto Bytes = instruction(C, 0, 0, true);
        const auto Exit = run(Bytes, std::move(Hooks));
        EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                  : ExecutionExitKind::Stopped)
            << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 0u);
        expectSnapshot(Expected);
        EXPECT_EQ(backing(), RAM);
        if (!Fail) {
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value =
              reference(C, input(C, One, Input), Flags, InitialMXCSR);
          Expected[CPURegister::X64FLAGS] = {Value.Flags, 0};
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
      }
}

TEST_P(X64SSEComparison, InstructionObserversStopBeforeRegisterEffects) {
  for (const auto &C : Comparisons)
    for (const auto &Input : {MinSubnormal, QuietNaN})
      for (bool Fail : {false, true}) {
        resetMemory();
        ASSERT_FALSE(HasFatalFailure());
        initialize(C, input(C, One, Input), InitialMXCSR);
        ASSERT_FALSE(HasFatalFailure());
        const auto Before = snapshot();
        const auto RAM = backing();
        const auto Bytes = instruction(C);
        ASSERT_EQ(llvm::toString(CPU->write(Code, Bytes)), "");
        unsigned Instructions = 0, Accesses = 0;
        BackendHooks Hooks;
        Hooks.Instruction = [&](uint64_t PC, unsigned Size) {
          EXPECT_EQ(PC, Code);
          EXPECT_EQ(Size, Bytes.size());
          ++Instructions;
          CPU->stop();
          if (Fail)
            throw std::runtime_error(ObserverError);
        };
        Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
        ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
        const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
        EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                                  : ExecutionExitKind::Stopped)
            << Exit.Diagnostic;
        EXPECT_EQ(Instructions, 1u);
        EXPECT_EQ(Accesses, 0u);
        expectSnapshot(Before);
        EXPECT_EQ(backing(), RAM);
        if (!Fail) {
          const auto Retry = run(Bytes);
          ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
          const auto Value =
              reference(C, input(C, One, Input), Flags, InitialMXCSR);
          auto Expected = Before;
          Expected[CPURegister::X64FLAGS] = {Value.Flags, 0};
          Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
          Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
          expectSnapshot(Expected);
          EXPECT_EQ(backing(), RAM);
        }
      }
}

TEST_P(X64SSEComparison, PageEndInputsUseOnlyTheFloatingSourceWidth) {
  for (const auto &C : Comparisons) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    const auto Width = sourceBytes(C);
    initialize(C, input(C, One, MinSubnormal), InitialMXCSR, 0, 0, true,
               Data + PageSize - Width);
    ASSERT_FALSE(HasFatalFailure());
    auto Expected = snapshot();
    const auto RAM = backing();
    ASSERT_EQ(
        llvm::toString(CPU->addressSpace()->unmap(Data + PageSize, PageSize)),
        "");
    const auto Bytes = instruction(C, 0, 0, true);
    const auto Exit = run(Bytes);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const auto Value =
        reference(C, input(C, One, MinSubnormal), Flags, InitialMXCSR);
    Expected[CPURegister::X64FLAGS] = {Value.Flags, 0};
    Expected[CPURegister::X64MXCSR] = {Value.MXCSR, 0};
    Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    expectSnapshot(Expected);
    EXPECT_EQ(backing(), RAM);
  }
}

TEST_P(X64SSEComparison, UnsupportedFormsRejectBeforeObservations) {
  auto Reject = [&](llvm::ArrayRef<uint8_t> Bytes) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Comparisons[0], input(Comparisons[0], One, MinSubnormal),
               InitialMXCSR);
    ASSERT_FALSE(HasFatalFailure());
    const auto Before = snapshot();
    const auto RAM = backing();
    unsigned Accesses = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
    const auto Exit = run(Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Accesses, 0u);
    expectSnapshot(Before);
    EXPECT_EQ(backing(), RAM);
  };
  for (const auto &C : Comparisons)
    for (bool Memory : {false, true}) {
      auto Bytes = instruction(C, 0, 0, Memory);
      Bytes.insert(Bytes.begin(), Lock);
      Reject(Bytes);
      ASSERT_FALSE(HasFatalFailure());
    }
#define NEVERD_SSE_COMPARE_INVALID(Name, ...)                                  \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Reject(Name);                                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_INVALID
}

TEST_P(X64SSEComparison, DeviceOperandsRejectBeforeCallbacks) {
  for (const auto &C : Comparisons) {
    resetMemory();
    ASSERT_FALSE(HasFatalFailure());
    initialize(C, input(C, One, MinSubnormal), InitialMXCSR, 0, 0, true, Stack);
    ASSERT_FALSE(HasFatalFailure());
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
      const auto Exit = run(instruction(C, 0, 0, true));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
    }
    EXPECT_EQ(Calls, 0u);
    expectSnapshot(Before);
    EXPECT_EQ(backing(), RAM);
  }
}

using X64SSEComparisonDAZ = daz_test::Fixture<X64SSEComparison>;
#define NEVERD_SSE_COMPARE_OPERATION(Name, Prefix, Opcode, Bits, Quiet)        \
  TEST_P(X64SSEComparisonDAZ, Name##FlagsAndStatusPreserveSources) {           \
    compare({#Name, Prefix, Opcode, Bits, Quiet}, daz_test::DenormalsAreZero); \
  }
#include "X64SSEComparisonCases.def"
#undef NEVERD_SSE_COMPARE_OPERATION
TEST_P(X64SSEComparisonDAZ, RoundingModesAndUnrelatedFlagsRemainIndependent) {
  rounding(daz_test::DenormalsAreZero);
}
INSTANTIATE_TEST_SUITE_P(DAZBackends, X64SSEComparisonDAZ,
                         testing::ValuesIn(daz_test::Parameters),
                         [](const auto &Info) { return Info.param.Name; });

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64SSEComparison,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

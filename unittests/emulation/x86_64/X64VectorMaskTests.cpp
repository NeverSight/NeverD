//===- X64VectorMaskTests.cpp - SSE masks and complete state retention ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"

#include <climits>
#include <stdexcept>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_MASK_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MASK_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_MASK_INVALID(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_INVALID
#undef NEVERD_MASK_TEXT
#undef NEVERD_MASK_VALUE
enum class Operation {
#define NEVERD_MASK_OPERATION(Name, ...) Name,
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_OPERATION
};
struct Mask {
  const char *Name;
  Operation Kind;
  unsigned Bits;
  uint8_t Prefix, Opcode;
};
constexpr Mask Masks[] = {
#define NEVERD_MASK_OPERATION(Name, Bits, Prefix, Opcode)                      \
  {#Name, Operation::Name, Bits, Prefix, Opcode},
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_OPERATION
};
constexpr CPURegister GeneralRegisters[] = {
#define NEVERD_MASK_REGISTER(Name) CPURegister::X64##Name,
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_REGISTER
};
constexpr RegisterValue Inputs[] = {
#define NEVERD_MASK_INPUT(Low, High) {Low, High},
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_INPUT
};

uint64_t scalarMask(unsigned Bits, RegisterValue Input) {
  uint64_t Result = 0;
  for (unsigned I = 0; I < VectorBytes * CHAR_BIT / Bits; ++I) {
    const unsigned Bit = (I + 1) * Bits - 1;
    Result |= ((Input[Bit / (WordBytes * CHAR_BIT)] >>
                (Bit % (WordBytes * CHAR_BIT))) &
               1)
              << I;
  }
  return Result;
}
void checkHost(const Mask &M, RegisterValue Input, uint64_t Expected) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Vector =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(Input.data()));
  int Result = 0;
  switch (M.Kind) {
  case Operation::MOVMSKPS:
    Result = _mm_movemask_ps(_mm_castsi128_ps(Vector));
    break;
  case Operation::MOVMSKPD:
    Result = _mm_movemask_pd(_mm_castsi128_pd(Vector));
    break;
  case Operation::PMOVMSKB:
    Result = _mm_movemask_epi8(Vector);
    break;
  }
  EXPECT_EQ(uint64_t(Result), Expected);
#else
  (void)M;
  (void)Input;
  (void)Expected;
#endif
}
std::vector<uint8_t> instruction(const Mask &M, unsigned Destination,
                                 unsigned Source, bool Wide) {
  std::vector<uint8_t> Bytes;
  if (M.Prefix)
    Bytes.push_back(M.Prefix);
  const uint8_t Extension =
      (Wide ? RexWide : 0) |
      (Destination > RegisterFieldMask ? RexDestination : 0) |
      (Source > RegisterFieldMask ? RexSource : 0);
  if (Extension)
    Bytes.push_back(Rex | Extension);
  Bytes.push_back(OpcodeMap);
  Bytes.push_back(M.Opcode);
  Bytes.push_back(RegisterModRM |
                  ((Destination & RegisterFieldMask) << RegisterFieldBits) |
                  (Source & RegisterFieldMask));
  return Bytes;
}

class X64VectorMask : public X64VectorTest {
protected:
  void initialize(RegisterValue Input, unsigned Source) {
    seed({{SentinelLow, SentinelHigh}, {SentinelHigh, SentinelLow}});
    for (unsigned I = 0; I < std::size(GeneralRegisters); ++I)
      llvm::cantFail(
          CPU->writeRegister(GeneralRegisters[I], {SentinelHigh + I, 0}));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setXmm(Source, Input));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FP0,
                                      {SentinelLow, PhysicalExponent}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FPTag, {1, 0}));
  }
  void expectRAM() {
    std::array<uint8_t, VectorBytes> Bytes{};
    ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Alias, Bytes)), "");
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), SentinelHigh);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + WordBytes),
              SentinelLow);
  }
  void check(const Mask &M, RegisterValue Input, unsigned Destination = 0,
             unsigned Source = 1, bool Wide = false) {
    SCOPED_TRACE(M.Name);
    SCOPED_TRACE(Destination);
    SCOPED_TRACE(Source);
    SCOPED_TRACE(Wide);
    const auto Expected = scalarMask(M.Bits, Input);
    checkHost(M, Input, Expected);
    initialize(Input, Source);
    auto State = snapshot();
    unsigned Observations = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Bytes = instruction(M, Destination, Source, Wide);
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    State[GeneralRegisters[Destination]] = {Expected, 0};
    State[CPURegister::X64PC] = {Code + Bytes.size(), 0};
    EXPECT_EQ(snapshot(), State);
    EXPECT_EQ(Observations, 0u);
    expectRAM();
  }
};

TEST_P(X64VectorMask, SignBitsMatchScalarAndHostResults) {
  for (const auto &M : Masks) {
    for (const auto &Input : Inputs) {
      check(M, Input);
      ASSERT_FALSE(HasFatalFailure());
    }
    for (unsigned Bit = 0; Bit < VectorBytes * CHAR_BIT; ++Bit) {
      RegisterValue Input{};
      Input[Bit / (WordBytes * CHAR_BIT)] = uint64_t(1)
                                            << (Bit % (WordBytes * CHAR_BIT));
      check(M, Input);
      ASSERT_FALSE(HasFatalFailure());
      for (auto &Word : Input)
        Word = ~Word;
      check(M, Input);
      ASSERT_FALSE(HasFatalFailure());
    }
  }
}

TEST_P(X64VectorMask, AllRegistersAndOperandWidthsZeroExtend) {
  for (const auto &M : Masks)
    for (unsigned D = 0; D < std::size(GeneralRegisters); ++D)
      for (unsigned S = 0; S < XmmCount; ++S)
        for (bool Wide : {false, true}) {
          check(M, Inputs[(D + S) % std::size(Inputs)], D, S, Wide);
          ASSERT_FALSE(HasFatalFailure());
        }
}

TEST_P(X64VectorMask, InvalidFormsRejectWithoutDataObservations) {
  struct Invalid {
    const char *Name;
    llvm::ArrayRef<uint8_t> Bytes;
  };
  const Invalid Instructions[] = {
#define NEVERD_MASK_INVALID(Name, ...) {#Name, Name},
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_INVALID
  };
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    reset();
    ASSERT_FALSE(HasFatalFailure());
    initialize(Inputs[0], 1);
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    const auto Before = snapshot();
    unsigned Observations = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Exit = run(I.Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(snapshot(), Before);
    expectRAM();
  }
}

TEST_P(X64VectorMask, InstructionStopsAndFailuresPreserveState) {
  for (const auto &M : Masks)
    for (bool Fail : {false, true}) {
      SCOPED_TRACE(M.Name);
      SCOPED_TRACE(Fail);
      reset();
      ASSERT_FALSE(HasFatalFailure());
      initialize(Inputs[0], 1);
      auto Before = snapshot();
      const auto Bytes = instruction(M, 0, 1, false);
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
      expectRAM();
      if (!Fail) {
        const auto Retry = run(Bytes);
        ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
        Before[CPURegister::X64AX] = {scalarMask(M.Bits, Inputs[0]), 0};
        Before[CPURegister::X64PC] = {Code + Bytes.size(), 0};
        EXPECT_EQ(snapshot(), Before);
        expectRAM();
      }
    }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64VectorMask,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation

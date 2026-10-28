//===- AArch64SIMDTests.cpp - Independent checked ARM64 FP/SIMD results ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_VALUE
struct FPCase {
  const char *Name;
  uint32_t Word;
  uint64_t First, Second, Result;
  uint32_t Control, Status;
};
constexpr FPCase FloatingCases[] = {
#define NEVERD_AARCH64_FP_INSTRUCTION(Name, Word, First, Second, Result,       \
                                      Control, Status)                         \
  {#Name, Word, First, Second, Result, Control, Status},
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_INSTRUCTION
};

class AArch64SIMD
    : public testing::TestWithParam<std::tuple<ExecutionBackendKind, bool>> {
protected:
  static constexpr unsigned ScalarCount = unsigned(AArch64Register::FPSR) + 1;
  using ScalarState = std::array<uint64_t, ScalarCount>;
  using VectorState = std::array<RegisterValue, LastVector + 1>;
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    const bool User = std::get<1>(GetParam());
    auto Created =
        createExecutionBackend(std::get<0>(GetParam()),
                               User ? ExecutionContract::CheckedUserAArch64
                                    : ExecutionContract::CheckedAArch64,
                               Limit, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->setReg(AArch64Register::SP, Stack + PageSize - StackAlignment));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialNZCV));
    for (unsigned Index = 0; Index <= LastVector; ++Index)
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, Index),
          {InitialVectorLow + Index, InitialVectorHigh - Index}));
  }
  RegisterValue vector(unsigned Index) {
    return llvm::cantFail(
        CPU->readRegister(vectorRegister(GuestArchitecture::AArch64, Index)));
  }
  void vector(unsigned Index, RegisterValue Value) {
    llvm::cantFail(CPU->writeRegister(
        vectorRegister(GuestArchitecture::AArch64, Index), Value));
  }
  ScalarState scalars() {
    ScalarState State;
    for (unsigned Index = 0; Index < State.size(); ++Index)
      State[Index] = llvm::cantFail(CPU->reg(AArch64Register(Index)));
    return State;
  }
  VectorState vectors() {
    VectorState State;
    for (unsigned Index = 0; Index < State.size(); ++Index)
      State[Index] = vector(Index);
    return State;
  }
  ExecutionExit run(uint32_t Word) {
    uint8_t Bytes[WordBytes];
    llvm::support::endian::write32le(Bytes, Word);
    llvm::support::endian::write32le(Bytes + sizeof(uint32_t), Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectSuccess(const ExecutionExit &Exit) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + sizeof(uint32_t));
  }
  void expectRejected(uint32_t Word) {
    auto ScalarBefore = scalars();
    const auto VectorBefore = vectors();
    ScalarBefore[unsigned(AArch64Register::PC)] = Code;
    const auto Exit = run(Word);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(vectors(), VectorBefore);
    EXPECT_EQ(scalars(), ScalarBefore);
  }
  void expectWrappedPairRejected(uint32_t Word, unsigned Width) {
    llvm::cantFail(CPU->map(LastPage, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->map(0, PageSize, Read | Write | UserAccessible));
    const auto Address = LastPage + PageSize - Width;
    llvm::cantFail(CPU->setReg(AArch64Register::X1, Address));
    std::array<uint8_t, ByteCount> Original{}, High{}, Low{};
    Original.fill(ByteMask);
    llvm::cantFail(
        CPU->write(Address, llvm::ArrayRef(Original).take_front(Width)));
    llvm::cantFail(CPU->write(0, Original));
    expectRejected(Word);
    llvm::cantFail(CPU->snapshotBacking(
        Address, llvm::MutableArrayRef(High).take_front(Width)));
    llvm::cantFail(CPU->snapshotBacking(0, Low));
    EXPECT_EQ(Low, Original);
    EXPECT_EQ(llvm::ArrayRef(High).take_front(Width),
              llvm::ArrayRef(Original).take_front(Width));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)), Address);
  }
};
TEST_P(AArch64SIMD, ExtractBytesConcatenatesBeforeWritingAndPreservesState) {
  struct ExtractCase {
    uint32_t Word;
    unsigned Bytes, Offset;
  };
  constexpr ExtractCase Cases[] = {
#define NEVERD_AARCH64_EXTRACT_CASE(Word, Bytes, Offset) {Word, Bytes, Offset},
#include "AArch64ExtractCases.def"
#undef NEVERD_AARCH64_EXTRACT_CASE
  };
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &C : Cases)
    for (unsigned First : {0u, 1u, 31u})
      for (unsigned Second : {0u, 1u, 31u})
        for (unsigned Destination : {0u, 1u, 31u}) {
          SCOPED_TRACE(C.Word);
          SCOPED_TRACE(First);
          SCOPED_TRACE(Second);
          SCOPED_TRACE(Destination);
          vector(First, {0x0123456789abcdefULL, 0xfedcba9876543210ULL});
          vector(Second, {0x08192a3b4c5d6e7fULL, 0xf0e1d2c3b4a59687ULL});
          auto ExpectedVectors = vectors();
          auto ExpectedScalars = scalars();
          ExpectedScalars[unsigned(AArch64Register::PC)] =
              Code + sizeof(uint32_t);
          // Read an independent byte window from the two complete source
          // snapshots. Narrow forms concatenate only their low eight bytes.
          RegisterValue Expected{};
          for (unsigned Byte = 0; Byte < C.Bytes; ++Byte) {
            const unsigned Index = C.Offset + Byte;
            const unsigned Source = Index < C.Bytes ? First : Second;
            const unsigned SourceByte = Index % C.Bytes;
            const uint8_t Value =
                ExpectedVectors[Source][SourceByte / WordBytes] >>
                ((SourceByte % WordBytes) * BytesPerBit);
            Expected[Byte / WordBytes] |= uint64_t(Value)
                                          << ((Byte % WordBytes) * BytesPerBit);
          }
          ExpectedVectors[Destination] = Expected;
          const uint32_t Word = (C.Word & ~0x1f03ffu) | (Second << 16) |
                                (First << 5) | Destination;
          ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
          EXPECT_EQ(vectors(), ExpectedVectors);
          EXPECT_EQ(scalars(), ExpectedScalars);
          std::array<uint8_t, 64> After;
          llvm::cantFail(CPU->snapshotBacking(Stack, After));
          EXPECT_EQ(After, Guard);
        }
}
// Each unsupported instruction needs a fresh checked execution fixture.
TEST_P(AArch64SIMD, ExtractBytesRejectsReservedNarrowOffsetEight) {
  expectRejected(0x2e02403f);
}
TEST_P(AArch64SIMD, ExtractBytesRejectsReservedNarrowOffsetFifteen) {
  expectRejected(0x2e02783f);
}
TEST_P(AArch64SIMD, ExtractBytesRejectsScalableOffsetZero) {
  expectRejected(0x05200020);
}
TEST_P(AArch64SIMD, ExtractBytesRejectsScalableOffsetMaximum) {
  expectRejected(0x053f1c20);
}
TEST_P(AArch64SIMD, Reverse64ElementsPreserveCompleteState) {
  struct Case {
    uint32_t Word;
    RegisterValue Result;
  };
  // Independently assembled 8B/16B/4H/8H/2S/4S forms. Each 64-bit block
  // reverses elements; it does not reverse the complete 128-bit register.
  constexpr Case Cases[] = {
      {0x0e20083f, {0xefcdab8967452301ULL, 0}},
      {0x4e20083f, {0xefcdab8967452301ULL, 0x1032547698badcfeULL}},
      {0x0e60083f, {0xcdef89ab45670123ULL, 0}},
      {0x4e60083f, {0xcdef89ab45670123ULL, 0x32107654ba98fedcULL}},
      {0x0ea0083f, {0x89abcdef01234567ULL, 0}},
      {0x4ea0083f, {0x89abcdef01234567ULL, 0x76543210fedcba98ULL}}};
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &C : Cases)
    for (unsigned Source : {0u, 1u, 31u})
      for (unsigned Destination : {0u, 1u, 31u}) {
        SCOPED_TRACE(C.Word);
        SCOPED_TRACE(Source);
        SCOPED_TRACE(Destination);
        vector(Source, {0x0123456789abcdefULL, 0xfedcba9876543210ULL});
        auto ExpectedVectors = vectors();
        ExpectedVectors[Destination] = C.Result;
        auto ExpectedScalars = scalars();
        ExpectedScalars[unsigned(AArch64Register::PC)] =
            Code + sizeof(uint32_t);
        const uint32_t Word = (C.Word & ~0x3ffu) | (Source << 5) | Destination;
        ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
        EXPECT_EQ(vectors(), ExpectedVectors);
        EXPECT_EQ(scalars(), ExpectedScalars);
        std::array<uint8_t, 64> After;
        llvm::cantFail(CPU->snapshotBacking(Stack, After));
        EXPECT_EQ(After, Guard);
      }
}
// An unsupported exit is terminal. Each negative word needs a fresh fixture.
TEST_P(AArch64SIMD, Reverse64RejectsReservedNarrowElementSize) {
  expectRejected(0x0ee0083f);
}
TEST_P(AArch64SIMD, Reverse64RejectsReservedWideElementSize) {
  expectRejected(0x4ee0083f);
}
// SVE REV Z31.B/H/S/D, Z1.B/H/S/D is a separate, unmodeled contract.
TEST_P(AArch64SIMD, Reverse64RejectsScalableBytes) {
  expectRejected(0x0538383f);
}
TEST_P(AArch64SIMD, Reverse64RejectsScalableHalfwords) {
  expectRejected(0x0578383f);
}
TEST_P(AArch64SIMD, Reverse64RejectsScalableWords) {
  expectRejected(0x05b8383f);
}
TEST_P(AArch64SIMD, Reverse64RejectsScalableDoublewords) {
  expectRejected(0x05f8383f);
}
TEST_P(AArch64SIMD, LogicalInstructionsUseBothIndependentLanes) {
  const auto First = vector(FirstOperand), Second = vector(SecondOperand);
  for (auto Word : {VectorXor, VectorAnd, VectorOr}) {
    SCOPED_TRACE(Word);
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
    const auto Actual = vector(LastVector);
    for (unsigned Lane = 0; Lane < Actual.size(); ++Lane) {
      const auto Expected = Word == VectorXor   ? First[Lane] ^ Second[Lane]
                            : Word == VectorAnd ? First[Lane] & Second[Lane]
                                                : First[Lane] | Second[Lane];
      EXPECT_EQ(Actual[Lane], Expected);
    }
    EXPECT_EQ(vector(FirstOperand), First);
    EXPECT_EQ(vector(SecondOperand), Second);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, PackedArithmeticWrapsEachByteWithoutCrossLaneCarry) {
  const auto First = vector(FirstOperand), Second = vector(SecondOperand);
  for (auto Word : {VectorAddBytes, VectorSubBytes}) {
    SCOPED_TRACE(Word);
    RegisterValue Expected{};
    for (unsigned Byte = 0; Byte < ByteCount; ++Byte) {
      const unsigned Lane = Byte / WordBytes;
      const unsigned Shift = (Byte % WordBytes) * BytesPerBit;
      const auto Left = uint8_t(First[Lane] >> Shift);
      const auto Right = uint8_t(Second[Lane] >> Shift);
      const auto Value = Word == VectorAddBytes ? uint8_t(Left + Right)
                                                : uint8_t(Left - Right);
      Expected[Lane] |= uint64_t(Value) << Shift;
    }
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
    EXPECT_EQ(vector(LastVector), Expected);
  }
}
TEST_P(AArch64SIMD, IntegerDuplicationFillsBothLanes) {
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InitialScalar));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(DuplicateInteger)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{InitialScalar, InitialScalar}));
}
TEST_P(AArch64SIMD, RegisterShiftsUseSignedLowByteAndPreserveFullState) {
  struct ShiftCase {
    uint32_t Word;
    unsigned Bits, Elements;
  };
  // Independently assembled USHL forms; clearing U selects SSHL. The last
  // encoding is scalar D, whose upper 64 bits are also cleared.
  constexpr ShiftCase Cases[] = {{0x2e22443f, 8, 8},  {0x6e22443f, 8, 16},
                                 {0x2e62443f, 16, 4}, {0x6e62443f, 16, 8},
                                 {0x2ea2443f, 32, 2}, {0x6ea2443f, 32, 4},
                                 {0x6ee2443f, 64, 2}, {0x7ee2443f, 64, 1}};
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &C : Cases)
    for (bool Signed : {false, true})
      for (unsigned Destination : {31u, 1u, 2u})
        for (unsigned Amount :
             {0u, 1u, C.Bits - 1, C.Bits, C.Bits + 1, 127u, 128u, 255u}) {
          SCOPED_TRACE(C.Word);
          SCOPED_TRACE(Signed);
          SCOPED_TRACE(Destination);
          SCOPED_TRACE(Amount);
          vector(31, {InitialVectorLow, InitialVectorHigh});
          vector(1, {0x80ff7f000102ff80ULL, 0xffffffff00000001ULL});
          RegisterValue Amounts{};
          const uint64_t Mask =
              C.Bits == 64 ? UINT64_MAX : (uint64_t(1) << C.Bits) - 1;
          for (unsigned E = 0; E < C.Elements; ++E) {
            // Nonzero upper bytes must not affect the shift amount.
            const uint64_t Lane =
                (0xa59701bcfa987600ULL | ((Amount + E) & 255)) & Mask;
            Amounts[E * C.Bits / 64] |= Lane << (E * C.Bits % 64);
          }
          vector(2, Amounts);
          auto ExpectedVectors = vectors();
          auto ExpectedScalars = scalars();
          ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
          RegisterValue Expected{};
          for (unsigned E = 0; E < C.Elements; ++E) {
            const unsigned W = E * C.Bits / 64, Offset = E * C.Bits % 64;
            const uint64_t Value = (ExpectedVectors[1][W] >> Offset) & Mask;
            const unsigned LowByte = (ExpectedVectors[2][W] >> Offset) & 255;
            const int Shift = LowByte < 128 ? int(LowByte) : int(LowByte) - 256;
            // Independent bit selection avoids host signed shifts and all
            // oversized-shift undefined behavior, including -128 and +127.
            for (unsigned B = 0; B < C.Bits; ++B) {
              const int Source = int(B) - Shift;
              const bool Bit = Source < 0 ? false
                               : Source >= int(C.Bits)
                                   ? Signed && ((Value >> (C.Bits - 1)) & 1)
                                   : (Value >> Source) & 1;
              Expected[W] |= uint64_t(Bit) << (Offset + B);
            }
          }
          ExpectedVectors[Destination] = Expected;
          const uint32_t Word = ((C.Word & ~31u) | Destination) &
                                (Signed ? ~(1u << 29) : UINT32_MAX);
          ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
          EXPECT_EQ(vectors(), ExpectedVectors);
          EXPECT_EQ(scalars(), ExpectedScalars);
          std::array<uint8_t, 64> After;
          llvm::cantFail(CPU->snapshotBacking(Stack, After));
          EXPECT_EQ(After, Guard);
        }
}
// A failed checked run is terminal; each rejection needs a fresh backend.
#define SHIFT_REJECTION(Name, Word)                                            \
  TEST_P(AArch64SIMD, RegisterShiftsReject##Name) { expectRejected(Word); }
SHIFT_REJECTION(UnsignedSingleLaneVector, 0x2ee2443f)
SHIFT_REJECTION(SignedSingleLaneVector, 0x0ee2443f)
SHIFT_REJECTION(UnsignedScalarByte, 0x7e22443f)
SHIFT_REJECTION(SignedScalarByte, 0x5e22443f)
SHIFT_REJECTION(UnsignedScalarHalf, 0x7e62443f)
SHIFT_REJECTION(SignedScalarHalf, 0x5e62443f)
SHIFT_REJECTION(UnsignedScalarWord, 0x7ea2443f)
SHIFT_REJECTION(SignedScalarWord, 0x5ea2443f)
#undef SHIFT_REJECTION
TEST_P(AArch64SIMD, WideningShiftsPreserveFullStateAndReadBeforeWriting) {
  struct WidenCase {
    uint32_t Word;
    unsigned Bits, Shift;
    bool High, Signed;
  };
  constexpr WidenCase Cases[] = {
#define NEVERD_AARCH64_WIDEN_CASE(Word, Bits, Shift, High, Signed)             \
  {Word, Bits, Shift, High, Signed},
#include "AArch64WidenCases.def"
#undef NEVERD_AARCH64_WIDEN_CASE
  };
  constexpr RegisterValue Inputs[] = {
      {0x80ff7f000102ff80ULL, 0xffffffff00000001ULL},
      {0x800000007fffffffULL, 0x800080007fff7fffULL},
      {0, UINT64_MAX},
  };
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    for (unsigned Destination : {31u, 1u}) {
      SCOPED_TRACE(Destination);
      for (const auto &Input : Inputs) {
        vector(31, {InitialVectorLow, InitialVectorHigh});
        vector(1, Input);
        auto ExpectedVectors = vectors();
        auto ExpectedScalars = scalars();
        ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
        RegisterValue Expected{};
        const auto Source = Input[Case.High ? 1 : 0];
        const auto SourceMask = (uint64_t(1) << Case.Bits) - 1;
        const unsigned WideBits = 2 * Case.Bits;
        const auto ResultMask =
            WideBits == 64 ? UINT64_MAX : (uint64_t(1) << WideBits) - 1;
        for (unsigned I = 0; I < 64 / Case.Bits; ++I) {
          const auto Lane = (Source >> (I * Case.Bits)) & SourceMask;
          // Multiplication in the widened signed domain avoids shifting a
          // negative C++ value. Even the largest product fits int64_t.
          const int64_t Extended =
              Case.Signed && (Lane & (uint64_t(1) << (Case.Bits - 1)))
                  ? int64_t(Lane) - (int64_t(1) << Case.Bits)
                  : int64_t(Lane);
          const auto Value =
              uint64_t(Extended * (int64_t(1) << Case.Shift)) & ResultMask;
          Expected[I * WideBits / 64] |= Value << (I * WideBits % 64);
        }
        ExpectedVectors[Destination] = Expected;
        ASSERT_NO_FATAL_FAILURE(
            expectSuccess(run((Case.Word & ~31u) | Destination)));
        EXPECT_EQ(vectors(), ExpectedVectors);
        EXPECT_EQ(scalars(), ExpectedScalars);
        std::array<uint8_t, 64> After;
        llvm::cantFail(CPU->snapshotBacking(Stack, After));
        EXPECT_EQ(After, Guard);
      }
    }
  }
}
TEST_P(AArch64SIMD, ZeroSizeFieldBelongsToModifiedImmediateInsteadOfWidening) {
  // immh == 0 belongs to a different encoding class: MOVI v31.4h, #1, LSL #8.
  auto ExpectedVectors = vectors();
  auto ExpectedScalars = scalars();
  ExpectedVectors[31] = {0x0100010001000100ULL, 0};
  ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(0x0f00a43f)));
  EXPECT_EQ(vectors(), ExpectedVectors);
  EXPECT_EQ(scalars(), ExpectedScalars);
}
TEST_P(AArch64SIMD, WideningRejects128BitElements) {
  expectRejected(0x0f40a43f);
}
TEST_P(AArch64SIMD, WideningRejectsScalarEncoding) {
  expectRejected(0x5f08a43f);
}
TEST_P(AArch64SIMD, LaneMovesUseNEONAlternativeAndPreserveOtherState) {
  struct LaneCase {
    uint32_t Word;
    unsigned Bits, Lane, ResultBits;
    bool Signed;
  };
  // Independently assembled with llvm-mc -triple=aarch64 -show-encoding.
  constexpr LaneCase Cases[] = {
      {0x0e013c3f, 8, 0, 32, false}, // UMOV WZR, V1.B[0]
      {0x0e1f3c3f, 8, 15, 32, false},
      {0x0e023c3f, 16, 0, 32, false}, // UMOV WZR, V1.H[0]
      {0x0e1e3c3f, 16, 7, 32, false},
      {0x0e043c3f, 32, 0, 32, false}, // MOV WZR, V1.S[0]
      {0x0e1c3c3f, 32, 3, 32, false},
      {0x4e083c3f, 64, 0, 64, false}, // MOV XZR, V1.D[0]
      {0x4e183c3f, 64, 1, 64, false},
      {0x0e012c3f, 8, 0, 32, true}, // SMOV WZR, V1.B[0]
      {0x0e1f2c3f, 8, 15, 32, true},
      {0x0e022c3f, 16, 0, 32, true},
      {0x0e1e2c3f, 16, 7, 32, true},
      {0x4e012c3f, 8, 0, 64, true},
      {0x4e1f2c3f, 8, 15, 64, true},
      {0x4e022c3f, 16, 0, 64, true},
      {0x4e1e2c3f, 16, 7, 64, true},
      {0x4e042c3f, 32, 0, 64, true},
      {0x4e1c2c3f, 32, 3, 64, true},
  };
  for (const auto Input :
       {RegisterValue{0, UINT64_MAX}, RegisterValue{UINT64_MAX, 0},
        RegisterValue{0x80ff7f000102ff80ULL, 0x817f8000ffffffffULL}}) {
    vector(1, Input);
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Word);
      for (unsigned Destination : {0u, 7u, 31u}) {
        llvm::cantFail(CPU->setReg(AArch64Register::X0, UINT64_MAX));
        llvm::cantFail(CPU->setReg(AArch64Register::X7, UINT64_MAX));
        auto ExpectedScalars = scalars();
        const auto ExpectedVectors = vectors();
        ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
        const auto Mask =
            Case.Bits == 64 ? UINT64_MAX : (uint64_t(1) << Case.Bits) - 1;
        auto Value = (Input[Case.Lane * Case.Bits / 64] >>
                      (Case.Lane * Case.Bits % 64)) &
                     Mask;
        if (Case.Signed && (Value & (uint64_t(1) << (Case.Bits - 1))))
          Value |= ~Mask;
        if (Destination != 31)
          ExpectedScalars[unsigned(AArch64Register::X0) + Destination] =
              Case.ResultBits == 32 ? uint32_t(Value) : Value;
        ASSERT_NO_FATAL_FAILURE(
            expectSuccess(run((Case.Word & ~31u) | Destination)));
        EXPECT_EQ(scalars(), ExpectedScalars);
        EXPECT_EQ(vectors(), ExpectedVectors);
      }
    }
  }
}
TEST_P(AArch64SIMD, CMHIProducesUnsignedElementMasksAndPreservesOtherState) {
  struct Comparison {
    const char *Name;
    uint32_t Word;
    unsigned Bits, Elements, Destination, Left, Right;
  };
  constexpr Comparison Cases[] = {
#define NEVERD_AARCH64_CMHI_CASE(Name, Word, Bits, Count, Dest, Left, Right)   \
  {#Name, Word, Bits, Count, Dest, Left, Right},
#include "AArch64CMHICases.def"
#undef NEVERD_AARCH64_CMHI_CASE
  };
  struct Operands {
    RegisterValue Left, Right;
  };
  constexpr Operands Inputs[] = {
      {{0x80ff7f000102ff80ULL, 0xffffffff00000001ULL},
       {0x7fff80000102fe80ULL, 0x7fffffff00000002ULL}},
      {{UINT64_MAX, 0x8000000000000000ULL}, {0, 0x7fffffffffffffffULL}},
      {{0, 0x7fffffffffffffffULL}, {UINT64_MAX, 0x8000000000000000ULL}},
      {{0x8877665544332211ULL, 0xfedcba9876543210ULL},
       {0x8877665544332211ULL, 0xfedcba9876543210ULL}},
  };
  for (unsigned Index = 0; Index <= unsigned(AArch64Register::X30); ++Index)
    llvm::cantFail(
        CPU->setReg(AArch64Register(Index), 0xabcdef0000000000ULL + Index));
  for (auto Register :
       {AArch64Register::TPIDR_EL0, AArch64Register::TPIDRRO_EL0,
        AArch64Register::TPIDR_EL1})
    llvm::cantFail(
        CPU->setReg(Register, 0x1234567800000000ULL + unsigned(Register)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> StackBefore;
  StackBefore.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, StackBefore));

  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    for (const auto &Input : Inputs) {
      for (unsigned Index = 0; Index <= LastVector; ++Index)
        vector(Index, {InitialVectorLow + Index, InitialVectorHigh - Index});
      vector(1, Input.Left);
      vector(2, Input.Right);
      auto ExpectedVectors = vectors();
      auto ExpectedScalars = scalars();
      ExpectedScalars[unsigned(AArch64Register::PC)] = Code + sizeof(uint32_t);
      const auto Left = ExpectedVectors[Case.Left];
      const auto Right = ExpectedVectors[Case.Right];
      const uint64_t Mask =
          Case.Bits == 64 ? UINT64_MAX : (uint64_t(1) << Case.Bits) - 1;
      RegisterValue Expected{};
      for (unsigned Element = 0; Element < Case.Elements; ++Element) {
        const unsigned Word = Element * Case.Bits / 64;
        const unsigned Shift = Element * Case.Bits % 64;
        const uint64_t A = (Left[Word] >> Shift) & Mask;
        const uint64_t B = (Right[Word] >> Shift) & Mask;
        if (A > B)
          Expected[Word] |= Mask << Shift;
      }
      ExpectedVectors[Case.Destination] = Expected;
      ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Case.Word)));
      EXPECT_EQ(vectors(), ExpectedVectors);
      EXPECT_EQ(scalars(), ExpectedScalars);
      std::array<uint8_t, 64> StackAfter;
      llvm::cantFail(CPU->snapshotBacking(Stack, StackAfter));
      EXPECT_EQ(StackAfter, StackBefore);
    }
  }
}
TEST_P(AArch64SIMD, SignedZeroComparisonPreservesFullState) {
  struct Comparison {
    uint32_t Word;
    unsigned Bits, Elements;
  };
  constexpr Comparison Cases[] = {
#define NEVERD_AARCH64_SIGNED_ZERO_CASE(Word, Bits, Count) {Word, Bits, Count},
#include "AArch64SignedZeroCases.def"
#undef NEVERD_AARCH64_SIGNED_ZERO_CASE
  };
  constexpr RegisterValue Inputs[] = {
      {0, 0},
      {UINT64_MAX, UINT64_MAX},
      {0x80ff7f000102ff80ULL, 0xffffffff00000001ULL},
      {0x8000000000000000ULL, 0x7fffffffffffffffULL}};
  for (unsigned Index = 0; Index <= unsigned(AArch64Register::X30); ++Index)
    llvm::cantFail(
        CPU->setReg(AArch64Register(Index), 0xabcdef0000000000ULL + Index));
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &C : Cases)
    for (unsigned Source : {0u, 1u, 31u})
      for (unsigned Destination : {0u, 1u, 31u})
        for (const auto &Input : Inputs) {
          SCOPED_TRACE(C.Word);
          SCOPED_TRACE(Source);
          SCOPED_TRACE(Destination);
          vector(Source, Input);
          auto ExpectedVectors = vectors();
          auto ExpectedScalars = scalars();
          ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
          const uint64_t Mask =
              C.Bits == 64 ? UINT64_MAX : (uint64_t(1) << C.Bits) - 1;
          RegisterValue Expected{};
          // An integer's sign bit selects the complete element mask. No host
          // signed conversion, floating-point operation or signed shift is
          // used.
          for (unsigned E = 0; E < C.Elements; ++E) {
            const unsigned Word = E * C.Bits / 64;
            const unsigned Shift = E * C.Bits % 64;
            if ((Input[Word] >> (Shift + C.Bits - 1)) & 1)
              Expected[Word] |= Mask << Shift;
          }
          ExpectedVectors[Destination] = Expected;
          const uint32_t Word =
              (C.Word & ~0x3ffu) | (Source << 5) | Destination;
          ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
          EXPECT_EQ(vectors(), ExpectedVectors);
          EXPECT_EQ(scalars(), ExpectedScalars);
          std::array<uint8_t, 64> After;
          llvm::cantFail(CPU->snapshotBacking(Stack, After));
          EXPECT_EQ(After, Guard);
        }
}
// Rejection is terminal, so every reserved/scalable word gets a fresh fixture.
TEST_P(AArch64SIMD, SignedZeroComparisonRejectsSingleDoubleVector) {
  expectRejected(0x0ee0a83f);
}
TEST_P(AArch64SIMD, SignedZeroComparisonRejectsScalarByte) {
  expectRejected(0x5e20a83f);
}
TEST_P(AArch64SIMD, SignedZeroComparisonRejectsScalarHalf) {
  expectRejected(0x5e60a83f);
}
TEST_P(AArch64SIMD, SignedZeroComparisonRejectsScalarWord) {
  expectRejected(0x5ea0a83f);
}
TEST_P(AArch64SIMD, SignedZeroComparisonRejectsScalablePredicate) {
  expectRejected(0x25002020);
}
TEST_P(AArch64SIMD, IntegerComparisonsAndReductionsPreserveFullState) {
  enum Operation { Equal, Zero, Unzip, Narrow, NarrowHigh, Maximum, Minimum };
  struct LaneCase {
    Operation Op;
    uint32_t Word;
    unsigned Bits, Elements;
  };
  constexpr LaneCase Cases[] = {
#define NEVERD_AARCH64_REDUCTION_CASE(Op, Word, Bits, Elements)                \
  {Op, Word, Bits, Elements},
#include "AArch64ReductionCases.def"
#undef NEVERD_AARCH64_REDUCTION_CASE
  };
  constexpr RegisterValue Left[] = {
      {0, 0},
      {UINT64_MAX, UINT64_MAX},
      {0x80ff7f000102ff80ULL, 0xffffffff00000001ULL},
      {0x1234000056780000ULL, 0x8000000000000000ULL},
      {0xfffefffdffffffffULL, 0x807f807f807f807fULL},
      {0x0203040506070809ULL, 0x1112131415161701ULL}};
  constexpr RegisterValue Right[] = {
      {UINT64_MAX, 0},
      {UINT64_MAX, UINT64_MAX},
      {0x7fff80000102fe80ULL, 0x7fffffff00000002ULL},
      {0x1234000056780000ULL, 0x8000000000000000ULL},
      {0xfffefffdffffffffULL, 0x807f807f807f807fULL},
      {0x0203040506070809ULL, 0x1112131415161701ULL}};
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    for (unsigned Destination : {31u, 1u, 2u}) {
      SCOPED_TRACE(Destination);
      for (unsigned Input = 0; Input != std::size(Left); ++Input) {
        SCOPED_TRACE(Input);
        vector(31, {InitialVectorLow, InitialVectorHigh});
        vector(1, Left[Input]);
        vector(2, Right[Input]);
        auto ExpectedVectors = vectors();
        auto ExpectedScalars = scalars();
        ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
        const unsigned Bits = Case.Bits, Count = Case.Elements;
        const uint64_t Mask =
            Bits == 64 ? UINT64_MAX : (uint64_t(1) << Bits) - 1;
        auto Element = [](RegisterValue Value, unsigned Index, unsigned Width) {
          const uint64_t Mask =
              Width == 64 ? UINT64_MAX : (uint64_t(1) << Width) - 1;
          return (Value[Index * Width / 64] >> (Index * Width % 64)) & Mask;
        };
        RegisterValue Expected{};
        if (Case.Op == Minimum)
          Expected[0] = Mask;
        if (Case.Op == NarrowHigh)
          Expected[0] = ExpectedVectors[Destination][0];
        for (unsigned I = 0; I < Count; ++I) {
          uint64_t Value = 0;
          if (Case.Op == Equal || Case.Op == Zero)
            Value =
                Element(Left[Input], I, Bits) ==
                        (Case.Op == Zero ? 0 : Element(Right[Input], I, Bits))
                    ? Mask
                    : 0;
          else if (Case.Op == Unzip)
            Value = Element(I < Count / 2 ? Left[Input] : Right[Input],
                            (I % (Count / 2)) * 2, Bits);
          else if (Case.Op == Narrow || Case.Op == NarrowHigh)
            Value = Element(Left[Input], I, Bits * 2) & Mask;
          else {
            Expected[0] =
                Case.Op == Minimum
                    ? std::min(Expected[0], Element(Left[Input], I, Bits))
                    : std::max(Expected[0], Element(Left[Input], I, Bits));
            continue;
          }
          const unsigned At = I * Bits + (Case.Op == NarrowHigh ? 64 : 0);
          Expected[At / 64] |= Value << (At % 64);
        }
        ExpectedVectors[Destination] = Expected;
        ASSERT_NO_FATAL_FAILURE(
            expectSuccess(run((Case.Word & ~31u) | Destination)));
        EXPECT_EQ(vectors(), ExpectedVectors);
        EXPECT_EQ(scalars(), ExpectedScalars);
        std::array<uint8_t, 64> After;
        llvm::cantFail(CPU->snapshotBacking(Stack, After));
        EXPECT_EQ(After, Guard);
      }
    }
  }
}
// Each refusal terminates its checked CPU; use a fresh fixture for each word.
#define REJECT_REDUCTION(Name, Word)                                           \
  TEST_P(AArch64SIMD, ReductionRejects##Name) { expectRejected(Word); }
REJECT_REDUCTION(SingleLaneCompare, 0x2ee28c3f)
REJECT_REDUCTION(ScalarByteCompare, 0x7e228c3f)
REJECT_REDUCTION(ScalarByteZeroCompare, 0x5e20983f)
REJECT_REDUCTION(SingleLaneUnzip, 0x0ec2183f)
REJECT_REDUCTION(NarrowBeyond64BitElements, 0x0ee1283f)
REJECT_REDUCTION(TwoLaneWordMaximum, 0x2eb0a83f)
REJECT_REDUCTION(DoubleWordMaximum, 0x6ef0a83f)
REJECT_REDUCTION(ScalableMaximum, 0x0409203f)
REJECT_REDUCTION(TwoLaneWordMinimum, 0x2eb1a83f)
REJECT_REDUCTION(DoubleWordMinimum, 0x6ef1a83f)
REJECT_REDUCTION(ScalableMinimum, 0x040b203f)
REJECT_REDUCTION(ScalableUnzip, 0x0522683f)
#undef REJECT_REDUCTION
TEST_P(AArch64SIMD, CMHIReservedSingleLaneVectorIsRejected) {
  expectRejected(0x2ee2343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarByteIsRejected) {
  expectRejected(0x7e22343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarHalfIsRejected) {
  expectRejected(0x7e62343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarWordIsRejected) {
  expectRejected(0x7ea2343f);
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonPreservesCompleteState) {
  struct Comparison {
    uint32_t Word;
    unsigned Bits, Elements;
  };
  // Independent llvm-mc encodings: S/D scalar and 2S/4S/2D vectors.
  constexpr Comparison Cases[] = {{0x7ea0d83f, 32, 1},
                                  {0x7ee0d83f, 64, 1},
                                  {0x2ea0d83f, 32, 2},
                                  {0x6ea0d83f, 32, 4},
                                  {0x6ee0d83f, 64, 2}};
  struct Input {
    RegisterValue Single, Double;
  };
  constexpr Input Inputs[] = {
      {{0x8000000000000000ULL, 0xbf8000003f800000ULL},
       {0, 0x8000000000000000ULL}},
      {{0xbf8000003f800000ULL, 0xff8000007f800000ULL},
       {0x3ff0000000000000ULL, 0xbff0000000000000ULL}},
      {{0xff8000007f800000ULL, 0xff7fffff7f7fffffULL},
       {0x7ff0000000000000ULL, 0xfff0000000000000ULL}},
      {{0xff7fffff7f7fffffULL, 0x8080000000800000ULL},
       {0x7fefffffffffffffULL, 0xffefffffffffffffULL}},
      {{0x8000000100000001ULL, 0x807fffff007fffffULL},
       {1, 0x8000000000000001ULL}},
      {{0x8080000000800000ULL, 0xbf8000003f800000ULL},
       {0x0010000000000000ULL, 0x8010000000000000ULL}},
      {{0xffc000007fc00000ULL, 0xff8000017f800001ULL},
       {0x7ff8000000000000ULL, 0xfff8000000000000ULL}},
      {{0xff8000017f800001ULL, 0x8000000000000000ULL},
       {0x7ff0000000000001ULL, 0xfff0000000000001ULL}},
      // Unused upper source lanes must not contribute an exception.
      {{0x7fc000003f800000ULL, 0x7f80000100000001ULL},
       {0x3ff0000000000000ULL, 0x7ff0000000000001ULL}},
  };
  constexpr uint32_t Controls[] = {0,
                                   1u << RoundingShift,
                                   2u << RoundingShift,
                                   3u << RoundingShift,
                                   FlushToZero,
                                   DefaultNaN,
                                   FlushToZero | DefaultNaN};
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    for (unsigned Destination : {0u, 1u, 31u}) {
      SCOPED_TRACE(Destination);
      for (const auto &Input : Inputs) {
        for (uint32_t Control : Controls) {
          SCOPED_TRACE(Control);
          vector(1, Case.Bits == 32 ? Input.Single : Input.Double);
          llvm::cantFail(CPU->setReg(AArch64Register::FPCR, Control));
          // Existing QC, inexact and divide-by-zero survive new IOC/IDC.
          constexpr uint32_t PriorStatus = 0x08000012;
          llvm::cantFail(CPU->setReg(AArch64Register::FPSR, PriorStatus));
          auto ExpectedVectors = vectors();
          auto ExpectedScalars = scalars();
          const auto Source = ExpectedVectors[1];
          RegisterValue Expected{};
          uint32_t Status = PriorStatus;
          const uint64_t Mask = Case.Bits == 64 ? UINT64_MAX : UINT32_MAX;
          const uint64_t Sign = uint64_t(1) << (Case.Bits - 1);
          const uint64_t Infinity =
              Case.Bits == 64 ? 0x7ff0000000000000ULL : 0x7f800000;
          const uint64_t Normal =
              Case.Bits == 64 ? 0x0010000000000000ULL : 0x00800000;
          // Ordered comparison to zero has an integer IEEE-bit oracle:
          // NaNs are unordered, both zeros and all negative numbers compare
          // true, and FZ turns either subnormal sign into the matching zero.
          for (unsigned Lane = 0; Lane != Case.Elements; ++Lane) {
            const unsigned Word = Lane * Case.Bits / 64;
            const unsigned Shift = Lane * Case.Bits % 64;
            const uint64_t Value = (Source[Word] >> Shift) & Mask;
            const uint64_t Magnitude = Value & ~Sign;
            const bool NaN = Magnitude > Infinity;
            const bool Flushed =
                (Control & FlushToZero) && Magnitude && Magnitude < Normal;
            if (NaN)
              Status |= InvalidOperation;
            if (Flushed)
              Status |= InputDenormal;
            if (!NaN && (!Magnitude || Flushed || (Value & Sign)))
              Expected[Word] |= Mask << Shift;
          }
          ExpectedVectors[Destination] = Expected;
          ExpectedScalars[unsigned(AArch64Register::FPSR)] = Status;
          ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
          ASSERT_NO_FATAL_FAILURE(
              expectSuccess(run((Case.Word & ~31u) | Destination)));
          EXPECT_EQ(vectors(), ExpectedVectors);
          EXPECT_EQ(scalars(), ExpectedScalars);
          std::array<uint8_t, 64> After;
          llvm::cantFail(CPU->snapshotBacking(Stack, After));
          EXPECT_EQ(After, Guard);
        }
      }
    }
  }
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonRejectsScalarHalf) {
  expectRejected(0x7ef8d83f);
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonRejectsFourHalves) {
  expectRejected(0x2ef8d83f);
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonRejectsEightHalves) {
  expectRejected(0x6ef8d83f);
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonRejectsScalableVector) {
  expectRejected(0x65d12030);
}
TEST_P(AArch64SIMD, FloatingZeroLEComparisonRejectsReservedOneDoubleVector) {
  expectRejected(0x2ee0d83f);
}
TEST_P(AArch64SIMD, FloatingConditionalComparisonChecksEveryConditionAndNZCV) {
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  // Independent llvm-mc encodings, with condition/NZCV fields initially zero.
  for (uint32_t Word : {0x1e220420u, 0x1e620420u, 0x1e220430u, 0x1e620430u}) {
    const bool Wide = Word & (1u << 22);
    vector(1, {Wide ? 0x3ff0000000000000ULL : 0x7f8000013f800000ULL,
               0x7ff0000000000001ULL});
    vector(2, {Wide ? 0x4000000000000000ULL : 0x7f80000140000000ULL,
               0x7ff0000000000001ULL});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0x08000012));
    for (unsigned Flags = 0; Flags != 16; ++Flags) {
      const bool N = Flags & 8, Z = Flags & 4, C = Flags & 2, V = Flags & 1;
      const bool Holds[] = {
          Z,    !Z,      C,       !C,     N,      !N,           V,
          !V,   C && !Z, !C || Z, N == V, N != V, !Z && N == V, Z || N != V,
          true, true};
      for (unsigned Condition = 0; Condition != 16; ++Condition) {
        const unsigned Fallback = (Flags + Condition) & 15;
        SCOPED_TRACE(testing::Message() << Word << ' ' << Flags << ' '
                                        << Condition << ' ' << Fallback);
        llvm::cantFail(
            CPU->setReg(AArch64Register::NZCV, uint64_t(Flags) << 28));
        auto ExpectedScalars = scalars();
        const auto ExpectedVectors = vectors();
        ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
        ExpectedScalars[unsigned(AArch64Register::NZCV)] =
            uint64_t(Holds[Condition] ? 8 : Fallback) << 28;
        ASSERT_NO_FATAL_FAILURE(
            expectSuccess(run(Word | (Condition << 12) | Fallback)));
        EXPECT_EQ(scalars(), ExpectedScalars);
        EXPECT_EQ(vectors(), ExpectedVectors);
        std::array<uint8_t, 64> After;
        llvm::cantFail(CPU->snapshotBacking(Stack, After));
        EXPECT_EQ(After, Guard);
      }
    }
    // Every literal NZCV pattern, including zero, on a false condition.
    for (unsigned Fallback = 0; Fallback != 16; ++Fallback) {
      llvm::cantFail(CPU->setReg(AArch64Register::NZCV, 0));
      auto ExpectedScalars = scalars();
      const auto ExpectedVectors = vectors();
      ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
      ExpectedScalars[unsigned(AArch64Register::NZCV)] = uint64_t(Fallback)
                                                         << 28;
      ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word | Fallback)));
      EXPECT_EQ(scalars(), ExpectedScalars);
      EXPECT_EQ(vectors(), ExpectedVectors);
    }
  }
}
TEST_P(AArch64SIMD,
       FloatingConditionalComparisonPreservesStateAndGatesExceptions) {
  struct Pair {
    uint32_t SingleA, SingleB;
    uint64_t DoubleA, DoubleB;
  };
  constexpr Pair Inputs[] = {
      {0x3f800000, 0x40000000, 0x3ff0000000000000ULL, 0x4000000000000000ULL},
      {0x40000000, 0x3f800000, 0x4000000000000000ULL, 0x3ff0000000000000ULL},
      {0x3f800000, 0x3f800000, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL},
      {0, 0x80000000, 0, 0x8000000000000000ULL},
      {0xbf800000, 0x3f800000, 0xbff0000000000000ULL, 0x3ff0000000000000ULL},
      {0xc0000000, 0xbf800000, 0xc000000000000000ULL, 0xbff0000000000000ULL},
      {0x7f7fffff, 0x7f800000, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL},
      {0xff800000, 0xff7fffff, 0xfff0000000000000ULL, 0xffefffffffffffffULL},
      {0x7f800000, 0x7f800000, 0x7ff0000000000000ULL, 0x7ff0000000000000ULL},
      {0x7fc00000, 0x3f800000, 0x7ff8000000000000ULL, 0x3ff0000000000000ULL},
      {0x3f800000, 0xffc00000, 0x3ff0000000000000ULL, 0xfff8000000000000ULL},
      {0x7f800001, 0x3f800000, 0x7ff0000000000001ULL, 0x3ff0000000000000ULL},
      {0x3f800000, 0xff800001, 0x3ff0000000000000ULL, 0xfff0000000000001ULL},
      {1, 0, 1, 0},
      {0x80000001, 0x80000000, 0x8000000000000001ULL, 0x8000000000000000ULL},
      {0x007fffff, 0x00800000, 0x000fffffffffffffULL, 0x0010000000000000ULL},
      {0x7fc00000, 1, 0x7ff8000000000000ULL, 1},
      {1, 0x7f800001, 1, 0x7ff0000000000001ULL},
  };
  constexpr uint32_t Controls[] = {0,
                                   1u << RoundingShift,
                                   2u << RoundingShift,
                                   3u << RoundingShift,
                                   FlushToZero,
                                   DefaultNaN,
                                   FlushToZero | DefaultNaN};
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  for (uint32_t Word : {0x1e220420u, 0x1e620420u, 0x1e220430u, 0x1e620430u}) {
    const bool Wide = Word & (1u << 22), Exceptional = Word & 16;
    const uint64_t Sign = uint64_t(1) << (Wide ? 63 : 31);
    const uint64_t Infinity = Wide ? 0x7ff0000000000000ULL : 0x7f800000;
    const uint64_t Normal = Wide ? 0x0010000000000000ULL : 0x00800000;
    const uint64_t Quiet = Normal >> 1;
    for (const auto &Input : Inputs) {
      for (uint32_t Control : Controls) {
        for (bool Take : {false, true}) {
          for (unsigned First : {1u, 31u}) {
            SCOPED_TRACE(testing::Message()
                         << Word << ' ' << Input.SingleA << ' ' << Input.SingleB
                         << ' ' << Control << ' ' << Take << ' ' << First);
            uint64_t A = Wide ? Input.DoubleA : Input.SingleA;
            uint64_t B = Wide ? Input.DoubleB : Input.SingleB;
            vector(First, {A | (Wide ? 0 : 0x7f80000100000000ULL),
                           0x7ff0000000000001ULL});
            vector(2, {B | (Wide ? 0 : 0x7f80000100000000ULL),
                       0x7ff0000000000001ULL});
            llvm::cantFail(CPU->setReg(AArch64Register::FPCR, Control));
            constexpr uint32_t PriorStatus = 0x08000012;
            llvm::cantFail(CPU->setReg(AArch64Register::FPSR, PriorStatus));
            llvm::cantFail(
                CPU->setReg(AArch64Register::NZCV, Take ? 0x40000000 : 0));
            auto ExpectedScalars = scalars();
            const auto ExpectedVectors = vectors();
            unsigned Flags = 5;
            uint32_t Status = PriorStatus;
            if (Take) {
              // Integer ordering of IEEE encodings, independent of host FP.
              auto Flush = [&](uint64_t Value) {
                const uint64_t Magnitude = Value & ~Sign;
                if ((Control & FlushToZero) && Magnitude &&
                    Magnitude < Normal) {
                  Status |= InputDenormal;
                  return Value & Sign;
                }
                return Value;
              };
              A = Flush(A);
              B = Flush(B);
              const bool NaNA = (A & ~Sign) > Infinity;
              const bool NaNB = (B & ~Sign) > Infinity;
              if (NaNA || NaNB) {
                Flags = 3;
                if (Exceptional || (NaNA && !(A & Quiet)) ||
                    (NaNB && !(B & Quiet)))
                  Status |= InvalidOperation;
              } else if (A == B || (!(A & ~Sign) && !(B & ~Sign))) {
                Flags = 6;
              } else {
                const bool Less = (A & Sign) != (B & Sign) ? bool(A & Sign)
                                  : A & Sign               ? A > B
                                                           : A < B;
                Flags = Less ? 8 : 2;
              }
            }
            ExpectedScalars[unsigned(AArch64Register::NZCV)] = uint64_t(Flags)
                                                               << 28;
            ExpectedScalars[unsigned(AArch64Register::FPSR)] = Status;
            ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
            ASSERT_NO_FATAL_FAILURE(
                expectSuccess(run((Word & ~(31u << 5)) | (First << 5) | 5)));
            EXPECT_EQ(scalars(), ExpectedScalars);
            EXPECT_EQ(vectors(), ExpectedVectors);
            std::array<uint8_t, 64> After;
            llvm::cantFail(CPU->snapshotBacking(Stack, After));
            EXPECT_EQ(After, Guard);
          }
        }
      }
    }
  }
}
TEST_P(AArch64SIMD, FloatingConditionalComparisonRejectsHalf) {
  expectRejected(0x1ee20420);
}
TEST_P(AArch64SIMD, FloatingConditionalComparisonRejectsExceptionalHalf) {
  expectRejected(0x1ee20430);
}
TEST_P(AArch64SIMD, FloatingConditionalComparisonRejectsReservedPrecision) {
  expectRejected(0x1ea20420);
}
TEST_P(AArch64SIMD,
       FloatingConditionalComparisonRejectsExceptionalReservedPrecision) {
  expectRejected(0x1ea20430);
}
TEST_P(AArch64SIMD, ScalarSIMDIntegerConversionsPreserveAllStateAndRounding) {
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  struct Conversion {
    uint32_t Word;
    uint64_t Input;
    std::array<uint64_t, 4> Results;
    uint32_t Status;
  };
  // Independently assembled SCVTF/UCVTF s31,s29 and d31,d29. Literal IEEE
  // results are ordered nearest-even, +infinity, -infinity, toward zero.
  // Poison above bit 31 distinguishes an S integer source from a D source.
  const Conversion Cases[] = {
      {0x5e21dbbf, 0, {0, 0, 0, 0}, 0},
      {0x5e21dbbf,
       UINT64_C(0xa5a5a5a5fffffffd),
       {0xc0400000, 0xc0400000, 0xc0400000, 0xc0400000},
       0},
      {0x5e21dbbf,
       0x80000000,
       {0xcf000000, 0xcf000000, 0xcf000000, 0xcf000000},
       0},
      {0x5e21dbbf,
       0x7fffffff,
       {0x4f000000, 0x4f000000, 0x4effffff, 0x4effffff},
       0x10},
      {0x7e21dbbf,
       UINT64_C(0xffffffff00000003),
       {0x40400000, 0x40400000, 0x40400000, 0x40400000},
       0},
      {0x7e21dbbf,
       0xffffffff,
       {0x4f800000, 0x4f800000, 0x4f7fffff, 0x4f7fffff},
       0x10},
      {0x5e21dbbf,
       0x1000001,
       {0x4b800000, 0x4b800001, 0x4b800000, 0x4b800000},
       0x10},
      {0x7e21dbbf,
       0x1000001,
       {0x4b800000, 0x4b800001, 0x4b800000, 0x4b800000},
       0x10},
      {0x5e21dbbf,
       0xfeffffff,
       {0xcb800000, 0xcb800000, 0xcb800001, 0xcb800000},
       0x10},
      {0x5e61dbbf,
       UINT64_C(0xfffffffffffffffd),
       {UINT64_C(0xc008000000000000), UINT64_C(0xc008000000000000),
        UINT64_C(0xc008000000000000), UINT64_C(0xc008000000000000)},
       0},
      {0x5e61dbbf,
       UINT64_C(0x8000000000000000),
       {UINT64_C(0xc3e0000000000000), UINT64_C(0xc3e0000000000000),
        UINT64_C(0xc3e0000000000000), UINT64_C(0xc3e0000000000000)},
       0},
      {0x7e61dbbf,
       UINT64_C(0x8000000000000000),
       {UINT64_C(0x43e0000000000000), UINT64_C(0x43e0000000000000),
        UINT64_C(0x43e0000000000000), UINT64_C(0x43e0000000000000)},
       0},
      {0x5e61dbbf,
       UINT64_C(0x7fffffffffffffff),
       {UINT64_C(0x43e0000000000000), UINT64_C(0x43e0000000000000),
        UINT64_C(0x43dfffffffffffff), UINT64_C(0x43dfffffffffffff)},
       0x10},
      {0x7e61dbbf,
       UINT64_MAX,
       {UINT64_C(0x43f0000000000000), UINT64_C(0x43f0000000000000),
        UINT64_C(0x43efffffffffffff), UINT64_C(0x43efffffffffffff)},
       0x10},
      {0x5e61dbbf,
       UINT64_C(0x20000000000001),
       {UINT64_C(0x4340000000000000), UINT64_C(0x4340000000000001),
        UINT64_C(0x4340000000000000), UINT64_C(0x4340000000000000)},
       0x10},
      {0x5e61dbbf,
       UINT64_C(0xffdfffffffffffff),
       {UINT64_C(0xc340000000000000), UINT64_C(0xc340000000000000),
        UINT64_C(0xc340000000000001), UINT64_C(0xc340000000000000)},
       0x10},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Word);
    SCOPED_TRACE(C.Input);
    for (unsigned Source : {29u, 31u}) {
      SCOPED_TRACE(Source);
      for (unsigned Mode = 0; Mode < C.Results.size(); ++Mode) {
        SCOPED_TRACE(Mode);
        vector(31, {InitialVectorLow, InitialVectorHigh});
        vector(Source, {C.Input, InitialVectorHigh});
        llvm::cantFail(
            CPU->setReg(AArch64Register::FPCR, Mode << RoundingShift));
        llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
        auto ExpectedVectors = vectors();
        auto ExpectedScalars = scalars();
        ExpectedVectors[31] = {C.Results[Mode], 0};
        ExpectedScalars[unsigned(AArch64Register::FPSR)] = C.Status | 2;
        ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
        const uint32_t Word = (C.Word & ~(31u << 5)) | (Source << 5);
        ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
        EXPECT_EQ(vectors(), ExpectedVectors);
        EXPECT_EQ(scalars(), ExpectedScalars);
        std::array<uint8_t, 64> After;
        llvm::cantFail(CPU->snapshotBacking(Stack, After));
        EXPECT_EQ(After, Guard);
      }
    }
  }
}
TEST_P(AArch64SIMD,
       ScalarSIMDIntegerConversionRestoresAliasedSourceAndFPState) {
  std::array<uint8_t, 64> Guard;
  Guard.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, Guard));
  vector(0, {UINT64_C(0x20000000000001), InitialVectorHigh});
  const uint64_t Control = (1u << RoundingShift) | (3u << 24);
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR, Control));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
  auto Context = llvm::cantFail(CPU->saveContext());
  auto ExpectedVectors = vectors();
  auto ExpectedScalars = scalars();
  ExpectedVectors[0] = {UINT64_C(0x4340000000000001), 0};
  ExpectedScalars[unsigned(AArch64Register::PC)] = Code + 4;
  ExpectedScalars[unsigned(AArch64Register::FPSR)] = 0x12;
  for (unsigned Attempt = 0; Attempt != 2; ++Attempt) {
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(0x5e61d800)));
    EXPECT_EQ(vectors(), ExpectedVectors);
    EXPECT_EQ(scalars(), ExpectedScalars);
    std::array<uint8_t, 64> After;
    llvm::cantFail(CPU->snapshotBacking(Stack, After));
    EXPECT_EQ(After, Guard);
    vector(0, {0, 0});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0));
    llvm::cantFail(CPU->restoreContext(*Context));
  }
}
// A rejected instruction terminates that CPU instance. Each boundary needs
// its own fixture so every original word is actually admitted and rejected.
TEST_P(AArch64SIMD, ScalarSIMDFixedPointSingleSignedRemainsRejected) {
  expectRejected(0x5f3fe7bf);
}
TEST_P(AArch64SIMD, ScalarSIMDFixedPointDoubleSignedRemainsRejected) {
  expectRejected(0x5f7fe7bf);
}
TEST_P(AArch64SIMD, ScalarSIMDFixedPointSingleUnsignedRemainsRejected) {
  expectRejected(0x7f3fe7bf);
}
TEST_P(AArch64SIMD, ScalarSIMDHalfSignedRemainsRejected) {
  expectRejected(0x5e79dbbf);
}
TEST_P(AArch64SIMD, ScalarSIMDHalfUnsignedRemainsRejected) {
  expectRejected(0x7e79dbbf);
}
TEST_P(AArch64SIMD, ScalarSIMDFloatToSignedIntegerRemainsRejected) {
  expectRejected(0x5ee1bbbf);
}
TEST_P(AArch64SIMD, ScalarSIMDFloatToUnsignedIntegerRemainsRejected) {
  expectRejected(0x7ea1bbbf);
}
TEST_P(AArch64SIMD, ScalarSIMDPackedSignedConversionRemainsRejected) {
  expectRejected(0x4e61dbbf);
}
TEST_P(AArch64SIMD, ScalarSIMDPackedUnsignedConversionRemainsRejected) {
  expectRejected(0x6e21dbbf);
}
TEST_P(AArch64SIMD, ScalarFloatingResultsRoundingAndStatusMatchIEEEBits) {
  for (const auto &Case : FloatingCases) {
    SCOPED_TRACE(Case.Name);
    vector(FirstOperand, {Case.First, InitialVectorHigh});
    vector(SecondOperand, {Case.Second, InitialVectorHigh});
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, Case.Control));
    const auto Sticky = InitialFPSR & ~InputDenormal;
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, Sticky));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Case.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{Case.Result, 0}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), Case.Control);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)),
              Sticky | Case.Status);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, UnsupportedFPStateCannotBeInstalledOrTruncated) {
  auto Control = CPU->setReg(AArch64Register::FPCR, InvalidFPControl);
  ASSERT_TRUE(bool(Control));
  llvm::consumeError(std::move(Control));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), 0u);
  auto Status = CPU->setReg(AArch64Register::FPSR, InvalidFPStatus);
  ASSERT_TRUE(bool(Status));
  llvm::consumeError(std::move(Status));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), 0u);
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleTwo, InitialVectorHigh});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleThree, 0}));
}
TEST_P(AArch64SIMD, HalfPrecisionRequiresAnExplicitExtendedProfile) {
  expectRejected(HalfPrecisionAdd);
}
TEST_P(AArch64SIMD, ScalarIntegerConversionsPreserveBitsRoundingAndStatus) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Control, Status;
  };
  // Independently assembled SCVTF/UCVTF s31,w0 and d31,x0. Results
  // explicitly distinguish signed input, W-register width and rounding.
  const Conversion Cases[] = {
      {0x1e22001f, UINT64_C(0xfffffffffffffffd), 0xc0400000, 0, 0},
      {0x1e23001f, UINT64_C(0xffffffff00000003), 0x40400000, 0, 0},
      {0x1e23001f, 0x1000001, 0x4b800000, 0, 0x10},
      {0x1e23001f, 0x1000001, 0x4b800001, 1u << RoundingShift, 0x10},
      {0x9e62001f, UINT64_C(0xfffffffffffffffd), UINT64_C(0xc008000000000000),
       0, 0},
      {0x9e63001f, UINT64_C(0x8000000000000000), UINT64_C(0x43e0000000000000),
       0, 0},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Word);
    llvm::cantFail(CPU->setReg(AArch64Register::X0, C.Input));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, C.Control));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{C.Output, 0}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), C.Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), C.Control);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, ScalarFloatConversionsSaturateAndKeepSourceState) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Status;
  };
  // FCVTZS/FCVTZU w0,s29 and x0,d29 use round-toward-zero, even when
  // FPCR requests round-up. W results must clear the upper word.
  const Conversion Cases[] = {
      {0x1e3803a0, 0xc0700000, 0xfffffffd, 0x10},
      {0x1e3903a0, 0x40700000, 3, 0x10},
      {0x1e3903a0, 0xbf800000, 0, 1},
      {0x1e3903a0, 0x7fc00000, 0, 1},
      {0x1e3903a0, 0x7f800000, 0xffffffff, 1},
      {0x9e7803a0, UINT64_C(0xc3e0000000000000), UINT64_C(0x8000000000000000),
       0},
      {0x9e7903a0, UINT64_C(0x7ff0000000000000), UINT64_MAX, 1},
      {0x9e7803a0, UINT64_C(0x7ff8000000000000), 0, 1},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Word);
    const RegisterValue Input{C.Input, InitialVectorHigh};
    vector(29, Input);
    llvm::cantFail(CPU->setReg(AArch64Register::X0, UINT64_MAX));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 1u << RoundingShift));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), C.Output);
    EXPECT_EQ(vector(29), Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, ScalarFloatResizeRetainsRoundingAndClearsUpperLanes) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Control, Status;
  };
  // FCVT d31,s29 and s31,d29.
  const Conversion Cases[] = {
      {0x1e22c3bf, 0x3fc00000, UINT64_C(0x3ff8000000000000), 0, 0},
      {0x1e6243bf, UINT64_C(0x3ff0000010000000), 0x3f800000, 0, 0x10},
      {0x1e6243bf, UINT64_C(0x3ff0000010000000), 0x3f800001,
       1u << RoundingShift, 0x10},
  };
  for (const auto &C : Cases) {
    const RegisterValue Input{C.Input, InitialVectorHigh};
    vector(29, Input);
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, C.Control));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{C.Output, 0}));
    EXPECT_EQ(vector(29), Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, HalfPrecisionConversionsRemainRejected) {
  expectRejected(0x1ee3001f);
}
TEST_P(AArch64SIMD, FixedPointConversionsRemainRejected) {
  expectRejected(0x1e03fc1f);
}
TEST_P(AArch64SIMD, PackedConversionsRemainRejected) {
  expectRejected(0x6e21dbbf);
}
TEST_P(AArch64SIMD, ScalableVectorsCannotEnterTheFixedWidthProfile) {
  expectRejected(ScalableVectorAdd);
}
TEST_P(AArch64SIMD, UnsupportedFPControlWriteCannotChangePublicState) {
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InvalidFPControl));
  expectRejected(WriteFPCR);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), 0u);
}
TEST_P(AArch64SIMD, ComparePublishesArchitecturalFlagsWithoutChangingVectors) {
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleTwo, InitialVectorHigh});
  const auto Before = vector(LastVector);
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(CompareDouble)));
  EXPECT_EQ(vector(LastVector), Before);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), LessNZCV);
}
TEST_P(AArch64SIMD, VectorFloatingAddPreservesIndependentDoubleLanes) {
  vector(FirstOperand, {DoubleOne, DoubleTwo});
  vector(SecondOperand, {DoubleTwo, DoubleOne});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(VectorFloatingAdd)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleThree, DoubleThree}));
}
TEST_P(AArch64SIMD,
       RestoredContextReinstallsVectorRoundingAndCumulativeStatus) {
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleHalfULP, InitialVectorHigh});
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 1u << RoundingShift));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  auto Context = llvm::cantFail(CPU->saveContext());
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleOne + 1, 0}));
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0));
  vector(SecondOperand, {DoubleOne, 0});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleTwo, 0}));
  llvm::cantFail(CPU->restoreContext(*Context));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleOne + 1, 0}));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)),
            1u << RoundingShift);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)),
            InitialFPSR | Inexact);
}
TEST_P(AArch64SIMD, PairVectorAddressWrapCannotAliasLowerPages) {
  expectWrappedPairRejected(PairVectorStore, ByteCount);
}
TEST_P(AArch64SIMD, PairIntegerAddressWrapCannotAliasLowerPages) {
  expectWrappedPairRejected(PairIntegerStore, WordBytes);
}
TEST_P(AArch64SIMD, FPControlInstructionsUseTheSamePublicState) {
  const auto Rounding = 1u << RoundingShift;
  llvm::cantFail(CPU->setReg(AArch64Register::X0, Rounding));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(WriteFPCR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), Rounding);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, 0));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(ReadFPCR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), Rounding);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InitialFPSR));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(WriteFPSR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), InitialFPSR);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, 0));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(ReadFPSR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), InitialFPSR);
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64SIMD,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation

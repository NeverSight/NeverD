//===- X87ArithmeticTests.cpp - Portable binary80 arithmetic tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bit fixtures, LLVM full-precision arithmetic and original x87
/// instruction references. A numerical pass does not qualify complete state.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/X87Arithmetic.h"
#include "neverd/ir/low/X87PartialRemainder.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"

#include <array>
#include <cstdint>
#include <random>
#include <utility>

#ifdef NEVERD_X87_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#include "unicorn/x86.h"
#endif

using namespace neverd;

namespace {
constexpr uint64_t IntegerBit = UINT64_C(1) << 63;
X87Binary80Bytes bits(uint64_t Significand, uint16_t Exponent) {
  X87Binary80Bytes Bytes{};
  for (unsigned I = 0; I != 8; ++I)
    Bytes[I] = Significand >> (8 * I);
  Bytes[8] = Exponent;
  Bytes[9] = Exponent >> 8;
  return Bytes;
}
llvm::APInt integer(const X87Binary80Bytes &Bytes) {
  llvm::APInt Value(80, 0);
  for (unsigned I = 0; I != Bytes.size(); ++I)
    Value |= llvm::APInt(80, Bytes[I]).shl(8 * I);
  return Value;
}
constexpr auto Add = X87ArithmeticOperation::Add;
constexpr auto Sub = X87ArithmeticOperation::Subtract;
constexpr auto Mul = X87ArithmeticOperation::Multiply;
constexpr auto Div = X87ArithmeticOperation::Divide;
constexpr std::array Operations{Add, Sub, Mul, Div};
uint16_t control(unsigned PC, unsigned RC) {
  return 0x7f | (PC << 8) | (RC << 10);
}

TEST(X87Arithmetic, SharedEncodingPreservesRawPayloadAndRemainderAdmission) {
  for (const auto &Value :
       {bits(0, 0x8000), bits(1, 0), bits(IntegerBit, 0), bits(1, 1),
        bits(UINT64_C(0xc000000000000123), 0xffff)}) {
    auto Decoded = decodeX87Binary80(Value);
    ASSERT_TRUE(Decoded);
    EXPECT_EQ(Decoded->bytes(), Value);
  }
  EXPECT_FALSE(decodeX87Binary80({}));
  const std::array<uint8_t, 9> Short{};
  EXPECT_FALSE(decodeX87Binary80(Short));
  // 7/2 has truncated quotient 3 and nearest/even quotient 4. Retain the
  // pre-existing partial-remainder owner's values and defined quotient bits.
  auto Truncated =
      evaluateX87PartialRemainder(bits(UINT64_C(0xe000000000000000), 0x4001),
                                  bits(IntegerBit, 0x4000), false);
  ASSERT_TRUE(Truncated);
  EXPECT_TRUE(Truncated->Complete);
  EXPECT_EQ(Truncated->Value, bits(IntegerBit, 0x3fff));
  EXPECT_EQ(Truncated->ConditionCodes, 0x4200);
  auto Nearest =
      evaluateX87PartialRemainder(bits(UINT64_C(0xe000000000000000), 0x4001),
                                  bits(IntegerBit, 0x4000), true);
  ASSERT_TRUE(Nearest);
  EXPECT_EQ(Nearest->Value, bits(IntegerBit, 0xbfff));
  EXPECT_EQ(Nearest->ConditionCodes, 0x0100);
  for (auto Unsupported : {bits(1, 0), bits(IntegerBit, 0), bits(1, 1),
                           bits(UINT64_C(0xc000000000000123), 0xffff)})
    EXPECT_FALSE(evaluateX87PartialRemainder(Unsupported,
                                             bits(IntegerBit, 0x4000), false));
}

TEST(X87Arithmetic, ExtendedPrecisionAndExponentAreNeverNarrowedToDouble) {
  const auto One = bits(IntegerBit, 0x3fff);
  const auto Tiny = bits(IntegerBit, 0x3fc0); // 2^-63, visible only at PC=64.
  auto Full = evaluateFiniteX87Arithmetic(Add, One, Tiny, control(3, 0));
  ASSERT_TRUE(Full);
  EXPECT_EQ(Full->Value, bits(IntegerBit + 1, 0x3fff));
  EXPECT_EQ(Full->Exceptions, 0);
  for (unsigned PC : {0U, 2U, 3U}) {
    auto Large =
        evaluateFiniteX87Arithmetic(Mul, bits(IntegerBit, 0x6000),
                                    bits(IntegerBit, 0x4000), control(PC, 0));
    ASSERT_TRUE(Large);
    EXPECT_EQ(Large->Value, bits(IntegerBit, 0x6001));
    EXPECT_EQ(Large->Exceptions, 0);
  }
}

TEST(X87Arithmetic, DirectRoundingDistinguishesBothSidesOfBinary80Midpoints) {
  for (unsigned PC : {0U, 2U}) {
    const unsigned Precision = PC == 0 ? 24 : 53;
    const uint64_t Step = UINT64_C(1) << (64 - Precision);
    // 1 + 2^-P + 2^(-P-63) lies strictly above the nearest/even midpoint.
    // First rounding to binary80 would lose the last term and round to 1.
    const auto AboveHalf = bits(IntegerBit + 1, 0x3fff - Precision);
    for (unsigned RC = 0; RC != 4; ++RC) {
      auto Positive = evaluateFiniteX87Arithmetic(Add, bits(IntegerBit, 0x3fff),
                                                  AboveHalf, control(PC, RC));
      ASSERT_TRUE(Positive);
      const bool Up = RC == 0 || RC == 2;
      EXPECT_EQ(Positive->Value, bits(IntegerBit + (Up ? Step : 0), 0x3fff));
      EXPECT_EQ(Positive->Exceptions, 32);
      EXPECT_EQ(Positive->RoundedUp, Up);
      auto NegativeHalf = AboveHalf;
      NegativeHalf[9] |= 0x80;
      auto Negative = evaluateFiniteX87Arithmetic(
          Add, bits(IntegerBit, 0xbfff), NegativeHalf, control(PC, RC));
      ASSERT_TRUE(Negative);
      const bool MagnitudeUp = RC == 0 || RC == 1;
      EXPECT_EQ(Negative->Value,
                bits(IntegerBit + (MagnitudeUp ? Step : 0), 0xbfff));
      EXPECT_EQ(Negative->RoundedUp, MagnitudeUp);
    }
    auto Tie = evaluateFiniteX87Arithmetic(Add, bits(IntegerBit, 0x3fff),
                                           bits(IntegerBit, 0x3fff - Precision),
                                           control(PC, 0));
    ASSERT_TRUE(Tie);
    EXPECT_EQ(Tie->Value, bits(IntegerBit, 0x3fff));
    EXPECT_FALSE(Tie->RoundedUp);
  }
}

TEST(X87Arithmetic, SignedZeroAndCancellationFollowRoundingDirection) {
  for (unsigned PC : {0U, 2U, 3U})
    for (unsigned RC = 0; RC != 4; ++RC) {
      auto Cancel = evaluateFiniteX87Arithmetic(Sub, bits(IntegerBit, 0x3fff),
                                                bits(IntegerBit, 0x3fff),
                                                control(PC, RC));
      ASSERT_TRUE(Cancel);
      EXPECT_EQ(Cancel->Value, bits(0, RC == 1 ? 0x8000 : 0));
      auto Negative = evaluateFiniteX87Arithmetic(
          Add, bits(0, 0x8000), bits(0, 0x8000), control(PC, RC));
      ASSERT_TRUE(Negative);
      EXPECT_EQ(Negative->Value, bits(0, 0x8000));
      auto Product = evaluateFiniteX87Arithmetic(
          Mul, bits(0, 0), bits(IntegerBit, 0xbfff), control(PC, RC));
      ASSERT_TRUE(Product);
      EXPECT_EQ(Product->Value, bits(0, 0x8000));
    }
}

TEST(X87Arithmetic, ZeroDivideTakesPriorityOverDenormalOperand) {
  for (unsigned PC : {0U, 2U, 3U})
    for (unsigned RC = 0; RC != 4; ++RC)
      for (unsigned SignA : {0U, 0x8000U})
        for (unsigned SignB : {0U, 0x8000U}) {
          auto Actual = evaluateFiniteX87Arithmetic(
              Div, bits(1, SignA), bits(0, SignB), control(PC, RC));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->Value, bits(IntegerBit, 0x7fff | (SignA ^ SignB)));
          EXPECT_EQ(Actual->Exceptions, 4);
          EXPECT_FALSE(Actual->RoundedUp);
        }
}

TEST(X87Arithmetic, FiniteComparisonMatchesIndependentLLVMOrdering) {
  std::mt19937_64 Random(0x87c01677);
  for (unsigned Trial = 0; Trial != 1024; ++Trial) {
    auto A = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    auto B = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    if (Trial % 8 == 0)
      A = bits(Random() & ~IntegerBit, Random() & 0x8000);
    if (Trial % 8 == 1)
      B = A;
    if (Trial % 8 == 2) {
      A = bits(0, 0);
      B = bits(0, 0x8000);
    }
    const llvm::APFloat Left(llvm::APFloat::x87DoubleExtended(), integer(A));
    const llvm::APFloat Right(llvm::APFloat::x87DoubleExtended(), integer(B));
    const auto Reference = Left.compare(Right);
    const auto Expected =
        Reference == llvm::APFloat::cmpLessThan ? X87Comparison::Less
        : Reference == llvm::APFloat::cmpEqual  ? X87Comparison::Equal
                                                : X87Comparison::Greater;
    auto Actual = evaluateFiniteX87Compare(A, B);
    ASSERT_TRUE(Actual);
    EXPECT_EQ(Actual->Relation, Expected) << Trial;
  }
  auto Denormal = evaluateFiniteX87Compare(bits(1, 0), bits(0, 0x8000));
  ASSERT_TRUE(Denormal);
  EXPECT_EQ(Denormal->Relation, X87Comparison::Greater);
  EXPECT_EQ(Denormal->Exceptions, 2);
  for (const auto &Unsupported :
       {bits(IntegerBit, 0x7fff), bits(UINT64_C(0xc000000000000321), 0xffff),
        bits(UINT64_C(0x8000000000003210), 0x7fff), bits(1, 0x4000)}) {
    EXPECT_FALSE(evaluateFiniteX87Compare(Unsupported, bits(0, 0)));
    EXPECT_FALSE(evaluateFiniteX87Compare(bits(0, 0), Unsupported));
  }
}

TEST(X87Arithmetic, IntegerStoresUseRCWithoutPCOrDenormalException) {
  for (unsigned Bytes : {2U, 4U, 8U})
    for (unsigned PC : {0U, 2U, 3U})
      for (unsigned RC = 0; RC != 4; ++RC)
        for (unsigned Sign : {0U, 0x8000U}) {
          const bool Negative = Sign != 0;
          const bool Away = RC == (Negative ? 1U : 2U);
          for (const auto &Input :
               {bits(1, Sign), bits(IntegerBit, 0x3ffe | Sign)}) {
            const auto Actual =
                convertFiniteX87ToInteger(Input, Bytes, control(PC, RC));
            ASSERT_TRUE(Actual);
            const uint64_t Expected = Away ? (Negative ? UINT64_MAX : 1) : 0;
            EXPECT_EQ(Actual->Bits,
                      llvm::APInt(Bytes * 8, Expected).getZExtValue());
            EXPECT_EQ(Actual->Exceptions, 32); // Never DE for FIST/FISTP.
            EXPECT_EQ(Actual->RoundedUp, Away);
          }
          const auto OneAndHalf =
              bits(UINT64_C(0xc000000000000000), 0x3fff | Sign);
          const auto Actual =
              convertFiniteX87ToInteger(OneAndHalf, Bytes, control(PC, RC));
          ASSERT_TRUE(Actual);
          const bool Up = RC == 0 || Away;
          const uint64_t Magnitude = Up ? 2 : 1;
          EXPECT_EQ(Actual->Bits,
                    llvm::APInt(Bytes * 8, Negative ? -Magnitude : Magnitude)
                        .getZExtValue());
          EXPECT_EQ(Actual->Exceptions, 32);
          EXPECT_EQ(Actual->RoundedUp, Up);
          // Both integer endpoints must survive even PC=24.
          const uint64_t SignBit = UINT64_C(1) << (Bytes * 8 - 1);
          for (uint64_t Exact : {SignBit, SignBit - 1}) {
            const auto Source = convertIntegerToX87(Exact, Bytes, true);
            ASSERT_TRUE(Source);
            const auto Endpoint =
                convertFiniteX87ToInteger(*Source, Bytes, control(PC, RC));
            ASSERT_TRUE(Endpoint);
            EXPECT_EQ(Endpoint->Bits, Exact);
            EXPECT_EQ(Endpoint->Exceptions, 0);
            EXPECT_FALSE(Endpoint->RoundedUp);
          }
        }
}

TEST(X87Arithmetic, IntegerStoreBoundariesMatchIndependentLLVMConversion) {
  constexpr std::array Rounding{
      llvm::APFloat::rmNearestTiesToEven, llvm::APFloat::rmTowardNegative,
      llvm::APFloat::rmTowardPositive, llvm::APFloat::rmTowardZero};
  std::mt19937_64 Random(0x87f1571677);
  for (unsigned Bytes : {2U, 4U, 8U})
    for (unsigned Trial = 0; Trial != 512; ++Trial) {
      // Concentrate on signed limits and fractional ties, with wide exponents
      // mixed in. LLVM's conversion independently supplies range/PE behavior.
      const unsigned Exponent = Trial % 4 == 0
                                    ? 1 + Random() % 0x7ffe
                                    : 0x3fff + Random() % (Bytes * 8 + 2);
      auto Value = bits(Random() | IntegerBit, Exponent | (Random() & 0x8000));
      if (Trial < 4)
        Value = bits(UINT64_MAX - (Trial & 1),
                     (0x3fff + Bytes * 8 - 2) | (Trial & 2 ? 0x8000 : 0));
      for (unsigned RC = 0; RC != 4; ++RC) {
        llvm::APSInt Converted(Bytes * 8, false);
        bool Exact = false;
        const llvm::APFloat Source(llvm::APFloat::x87DoubleExtended(),
                                   integer(Value));
        const auto Status =
            Source.convertToInteger(Converted, Rounding[RC], &Exact);
        const bool Invalid = Status & llvm::APFloat::opInvalidOp;
        const uint64_t Expected =
            Invalid ? UINT64_C(1) << (Bytes * 8 - 1) : Converted.getZExtValue();
        for (unsigned PC : {0U, 2U, 3U}) {
          const auto Actual =
              convertFiniteX87ToInteger(Value, Bytes, control(PC, RC));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->Bits, Expected);
          EXPECT_EQ(Actual->Exceptions, Invalid ? 1 : Exact ? 0 : 32);
          if (Invalid)
            EXPECT_FALSE(Actual->RoundedUp);
        }
      }
    }
  for (const auto &Unsupported :
       {bits(IntegerBit, 0x7fff), bits(UINT64_C(0xc000000000000123), 0x7fff),
        bits(1, 1), bits(IntegerBit, 0)})
    EXPECT_FALSE(convertFiniteX87ToInteger(Unsupported, 4, 0x37f));
  EXPECT_FALSE(convertFiniteX87ToInteger(bits(0, 0), 1, 0x37f));
  EXPECT_FALSE(convertFiniteX87ToInteger(bits(0, 0), 4, 0x17f));
}

TEST(X87Arithmetic, OverflowAndGradualUnderflowRetainPCAndStatus) {
  for (unsigned PC : {0U, 2U, 3U}) {
    const unsigned P = PC == 0 ? 24 : PC == 2 ? 53 : 64;
    for (unsigned RC = 0; RC != 4; ++RC) {
      auto Big = evaluateFiniteX87Arithmetic(Mul, bits(IntegerBit, 0x7ffe),
                                             bits(IntegerBit, 0x4000),
                                             control(PC, RC));
      ASSERT_TRUE(Big);
      const bool Infinite = RC == 0 || RC == 2;
      EXPECT_EQ(Big->Value, Infinite ? bits(IntegerBit, 0x7fff)
                                     : bits(~UINT64_C(0) << (64 - P), 0x7ffe));
      EXPECT_EQ(Big->Exceptions, 40);
      auto HalfMinimum = evaluateFiniteX87Arithmetic(
          Div, bits(IntegerBit, 1), bits(IntegerBit, 0x4000), control(PC, RC));
      ASSERT_TRUE(HalfMinimum);
      EXPECT_EQ(HalfMinimum->Value, bits(IntegerBit >> 1, 0));
      EXPECT_EQ(HalfMinimum->Exceptions,
                0); // exact tiny output, masked profile
      auto BelowMinimum = evaluateFiniteX87Arithmetic(
          Div, bits(1, 0), bits(IntegerBit, 0x4000), control(PC, RC));
      ASSERT_TRUE(BelowMinimum);
      EXPECT_EQ(BelowMinimum->Value,
                bits(RC == 2 ? UINT64_C(1) << (64 - P) : 0, 0));
      EXPECT_EQ(BelowMinimum->Exceptions, 2 | 16 | 32);
    }
  }
}

TEST(X87Arithmetic, InvalidAndUnsupportedDomainsRemainExplicit) {
  const auto One = bits(IntegerBit, 0x3fff);
  for (auto InvalidInput :
       {bits(0, 1), bits(IntegerBit, 0), bits(IntegerBit, 0x7fff),
        bits(IntegerBit + 1, 0x7fff),
        bits(UINT64_C(0xc000000000000123), 0x7fff)})
    EXPECT_FALSE(
        evaluateFiniteX87Arithmetic(Add, InvalidInput, One, control(3, 0)));
  for (uint16_t CW : {uint16_t(0x17f), uint16_t(0x837f), uint16_t(0x03ff)})
    EXPECT_FALSE(evaluateFiniteX87Arithmetic(Add, One, One, CW));
  EXPECT_FALSE(evaluateFiniteX87Arithmetic(
      static_cast<X87ArithmeticOperation>(99), One, One, control(3, 0)));
  auto Invalid =
      evaluateFiniteX87Arithmetic(Div, bits(0, 0), bits(0, 0), control(3, 0));
  ASSERT_TRUE(Invalid);
  EXPECT_EQ(Invalid->Value, bits(UINT64_C(0xc000000000000000), 0xffff));
  EXPECT_EQ(Invalid->Exceptions, 1);
  auto ZeroDivide =
      evaluateFiniteX87Arithmetic(Div, One, bits(0, 0x8000), control(3, 0));
  ASSERT_TRUE(ZeroDivide);
  EXPECT_EQ(ZeroDivide->Value, bits(IntegerBit, 0xffff));
  EXPECT_EQ(ZeroDivide->Exceptions, 4);
}

TEST(X87Arithmetic, FullPrecisionBitsMatchIndependentLLVMArithmetic) {
  std::mt19937_64 Random(0x87a1677);
  const llvm::APFloat::roundingMode Rounding[] = {
      llvm::APFloat::rmNearestTiesToEven, llvm::APFloat::rmTowardNegative,
      llvm::APFloat::rmTowardPositive, llvm::APFloat::rmTowardZero};
  for (unsigned Trial = 0; Trial != 256; ++Trial) {
    auto A = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    auto B = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    for (unsigned RC = 0; RC != 4; ++RC)
      for (auto Op : Operations) {
        SCOPED_TRACE(testing::Message()
                     << Trial << ':' << RC << ':' << unsigned(Op));
        llvm::APFloat L(llvm::APFloat::x87DoubleExtended(), integer(A));
        llvm::APFloat R(llvm::APFloat::x87DoubleExtended(), integer(B));
        llvm::APFloat::opStatus Status = llvm::APFloat::opOK;
        switch (Op) {
        case Add:
          Status = L.add(R, Rounding[RC]);
          break;
        case Sub:
          Status = L.subtract(R, Rounding[RC]);
          break;
        case Mul:
          Status = L.multiply(R, Rounding[RC]);
          break;
        case Div:
          Status = L.divide(R, Rounding[RC]);
          break;
        }
        auto Result = evaluateFiniteX87Arithmetic(Op, A, B, control(3, RC));
        ASSERT_TRUE(Result);
        EXPECT_EQ(integer(Result->Value), L.bitcastToAPInt());
        const unsigned Flags =
            ((Status & llvm::APFloat::opInvalidOp) ? 1 : 0) |
            ((Status & llvm::APFloat::opDivByZero) ? 4 : 0) |
            ((Status & llvm::APFloat::opOverflow) ? 8 : 0) |
            ((Status & llvm::APFloat::opUnderflow) ? 16 : 0) |
            ((Status & llvm::APFloat::opInexact) ? 32 : 0);
        // LLVM handleOverflow deliberately reports only opInexact when it
        // saturates to the largest finite value. It cannot qualify x87 OE in
        // that case; original-instruction replay and boundary fixtures do.
        const bool Saturated = L.isLargest() && RC != 0 &&
                               (RC == 3 || (RC == 1 && !L.isNegative()) ||
                                (RC == 2 && L.isNegative()));
        const unsigned ComparedMask = Saturated ? 0x37 : 0x3f;
        EXPECT_EQ(Result->Exceptions & ComparedMask, Flags & ComparedMask);
      }
  }
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
std::pair<X87Binary80Bytes, uint16_t>
nativeArithmetic(X87ArithmeticOperation Op, const X87Binary80Bytes &A,
                 const X87Binary80Bytes &B, uint16_t Control) {
  alignas(16) std::array<uint8_t, 512> Host{};
  X87Binary80Bytes Result{};
  uint16_t Status = 0;
#define NEVERD_X87_ORIGINAL(Instruction)                                       \
  __asm__ volatile(                                                            \
      "fxsave64 %0\n\tfninit\n\tfldcw %3\n\tfldt %5\n\tfldt "                  \
      "%4\n\t" Instruction                                                     \
      " %%st(1), %%st(0)\n\tfnstsw %2\n\tfstpt %1\n\tfxrstor64 %0"             \
      : "=m"(Host), "=m"(Result), "=m"(Status)                                 \
      : "m"(Control), "m"(A), "m"(B)                                           \
      : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",  \
        "st(7)")
  switch (Op) {
  case Add:
    NEVERD_X87_ORIGINAL("fadd");
    break;
  case Sub:
    NEVERD_X87_ORIGINAL("fsub");
    break;
  case Mul:
    NEVERD_X87_ORIGINAL("fmul");
    break;
  case Div:
    NEVERD_X87_ORIGINAL("fdiv");
    break;
  }
#undef NEVERD_X87_ORIGINAL
  return {Result, Status};
}
#endif

TEST(X87Arithmetic, NativeOriginalBitsAndDefinedArithmeticStatus) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  std::mt19937_64 Random(0x87a1677);
  for (unsigned Trial = 0; Trial != 128; ++Trial) {
    auto A = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    auto B = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    if (Trial % 8 == 0)
      A = bits(Random() & ~IntegerBit, Random() & 0x8000);
    if (Trial % 8 == 1)
      B = bits(0, Random() & 0x8000);
    for (unsigned PC : {0U, 2U, 3U})
      for (unsigned RC = 0; RC != 4; ++RC)
        for (auto Op : Operations) {
          SCOPED_TRACE(testing::Message() << Trial << ':' << PC << ':' << RC
                                          << ':' << unsigned(Op));
          auto Expected = nativeArithmetic(Op, A, B, control(PC, RC));
          auto Actual = evaluateFiniteX87Arithmetic(Op, A, B, control(PC, RC));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->Value, Expected.first);
          EXPECT_EQ(Actual->Exceptions, Expected.second & 0x3f);
          EXPECT_EQ(Actual->RoundedUp, (Expected.second & 0x200) != 0);
        }
  }
#else
  GTEST_SKIP()
      << "native x86-64 GCC/Clang original-instruction oracle required";
#endif
}

#ifdef NEVERD_X87_UNICORN_REFERENCE
TEST(X87Arithmetic, AllPrecisionsMatchIndependentX86InstructionsInUnicorn) {
  uc_engine *UC = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &UC), UC_ERR_OK);
  struct Close {
    uc_engine *UC;
    ~Close() { uc_close(UC); }
  } Guard{UC};
  ASSERT_EQ(uc_ctl_set_cpu_model(UC, UC_CPU_X86_HASWELL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, 0x1000, 4096, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, 0x2000, 4096, UC_PROT_READ | UC_PROT_WRITE),
            UC_ERR_OK);
  // FNINIT; MOV RAX,0x2000; FLDCW [RAX]; FLD m80 [RAX+0x20];
  // FLD m80 [RAX+0x10]; arithmetic ST0,ST1; FNSTSW [RAX+0x40]; FSTP m80.
  std::array<uint8_t, 26> Code = {0xdb, 0xe3, 0xb8, 0x00, 0x20, 0x00, 0x00,
                                  0xd9, 0x28, 0xdb, 0x68, 0x20, 0xdb, 0x68,
                                  0x10, 0xd8, 0xc1, 0xdd, 0x78, 0x40, 0xdb,
                                  0x78, 0x30, 0x90, 0x90, 0x90};
  std::mt19937_64 Random(0x87a1677);
  for (unsigned Trial = 0; Trial != 128; ++Trial) {
    auto A = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    auto B = bits(Random() | IntegerBit,
                  uint16_t(1 + Random() % 0x7ffe) | (Random() & 0x8000));
    for (unsigned PC : {0U, 2U, 3U})
      for (unsigned RC = 0; RC != 4; ++RC)
        for (auto Op : Operations) {
          SCOPED_TRACE(testing::Message() << Trial << ':' << PC << ':' << RC
                                          << ':' << unsigned(Op));
          Code[16] = Op == Add   ? 0xc1
                     : Op == Sub ? 0xe1
                     : Op == Mul ? 0xc9
                                 : 0xf1;
          ASSERT_EQ(uc_mem_write(UC, 0x1000, Code.data(), Code.size()),
                    UC_ERR_OK);
          ASSERT_EQ(uc_ctl_remove_cache(UC, 0x1000, 0x1000 + Code.size()),
                    UC_ERR_OK);
          const std::array<uint8_t, 2> CW{uint8_t(control(PC, RC)),
                                          uint8_t(control(PC, RC) >> 8)};
          ASSERT_EQ(uc_mem_write(UC, 0x2000, CW.data(), CW.size()), UC_ERR_OK);
          ASSERT_EQ(uc_mem_write(UC, 0x2010, A.data(), A.size()), UC_ERR_OK);
          ASSERT_EQ(uc_mem_write(UC, 0x2020, B.data(), B.size()), UC_ERR_OK);
          ASSERT_EQ(uc_emu_start(UC, 0x1000, 0x1000 + Code.size(), 0, 16),
                    UC_ERR_OK);
          X87Binary80Bytes Expected{};
          ASSERT_EQ(uc_mem_read(UC, 0x2030, Expected.data(), Expected.size()),
                    UC_ERR_OK);
          auto Result = evaluateFiniteX87Arithmetic(Op, A, B, control(PC, RC));
          ASSERT_TRUE(Result);
          EXPECT_EQ(Result->Value, Expected);
        }
  }
  // Unicorn is an independent numerical reference, not native CPU or C1/OS
  // exception evidence. Its model/revision must remain visible in run records.
  RecordProperty(
      "reference",
      "Unicorn x86-64 Haswell original instructions; numerical bits only");
}
#endif
} // namespace

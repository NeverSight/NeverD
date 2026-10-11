//===- X86_64_FPMinMaxAccuracyTests.cpp - MIN/MAX completion tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#endif

using namespace neverd;

namespace {
LowOp minMaxOp(bool Maximum, bool Double, bool Scalar, unsigned Bytes,
               uint64_t Mask, bool SAE = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::reg(10000, Bytes);
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X86FPArith), 2));
  Op.addInput(NdVar::cst(
      makeX86FPArithControl(Maximum ? X86FPArithKind::Maximum
                                    : X86FPArithKind::Minimum,
                            Double, Scalar, SAE, X86FPRounding::MXCSR),
      2));
  Op.addInput(NdVar::reg(10001, Bytes));
  Op.addInput(NdVar::reg(10002, Bytes));
  Op.addInput(NdVar::cst(0, Bytes));
  const auto Lanes = Scalar ? 1 : Bytes / (Double ? 8 : 4);
  Op.addInput(NdVar::cst(Mask, (Lanes + 7) / 8));
  return Op;
}

std::vector<uint8_t> repeated(uint64_t Bits, unsigned Element, unsigned Bytes) {
  std::vector<uint8_t> Result(Bytes, 0);
  for (unsigned Offset = 0; Offset < Bytes; Offset += Element)
    for (unsigned Index = 0; Index < Element; ++Index)
      Result[Offset + Index] = uint8_t(Bits >> (Index * 8));
  return Result;
}

TEST(X86FPMinMaxContract, DenormalSelectionNeitherFlushesNorRaisesUnderflow) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Double : {false, true})
    for (bool Maximum : {false, true})
      for (bool Scalar : {false, true})
        for (unsigned Bytes : {16U, 32U, 64U}) {
          if (Scalar && Bytes != 16)
            continue;
          const unsigned Element = Double ? 8 : 4;
          const unsigned Lanes = Scalar ? 1 : Bytes / Element;
          const uint64_t Sign =
              Double ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
          const uint64_t One =
              Double ? UINT64_C(0x3ff0000000000000) : UINT64_C(0x3f800000);
          const uint64_t Selected = 1 | (Maximum ? Sign : 0);
          const auto Left =
              repeated(One | (Maximum ? Sign : 0), Element, Bytes);
          const auto Right = repeated(Selected, Element, Bytes);
          auto Expected = Right;
          if (Scalar)
            std::fill(Expected.begin() + Element, Expected.end(), 0);
          for (bool FTZ : {false, true})
            for (bool UnderflowMasked : {false, true}) {
              const uint32_t Incoming =
                  (0x1f80U & ~(UnderflowMasked ? 0 : 0x800U)) |
                  (FTZ ? 0x8000U : 0);
              SCOPED_TRACE(testing::Message()
                           << "double=" << Double << " max=" << Maximum
                           << " scalar=" << Scalar << " bytes=" << Bytes
                           << " csr=" << Incoming);
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              Emulator.setMXCSR(Incoming);
              Emulator.setRegisterBytes(10001, Left);
              Emulator.setRegisterBytes(10002, Right);
              Emulator.setRegisterBytes(10000,
                                        std::vector<uint8_t>(Bytes, 0x59));
              EXPECT_TRUE(Emulator.step(minMaxOp(Maximum, Double, Scalar, Bytes,
                                                 (UINT64_C(1) << Lanes) - 1)));
              EXPECT_EQ(Emulator.getRegisterBytes(10000), Expected);
              EXPECT_EQ(Emulator.getMXCSR(), Incoming | 2U);
            }
        }
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
uint64_t nativeMinMax(uint64_t A, uint64_t B, bool Double, bool Maximum,
                      uint32_t &State) {
  const uint32_t Saved = _mm_getcsr();
  uint64_t Bits = 0;
  if (Double) {
    double Left, Right;
    std::memcpy(&Left, &A, 8);
    std::memcpy(&Right, &B, 8);
    if (Maximum)
      __asm__ volatile("ldmxcsr %1\n\tmaxsd %2,%0\n\tstmxcsr %1"
                       : "+x"(Left), "+m"(State)
                       : "x"(Right)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tminsd %2,%0\n\tstmxcsr %1"
                       : "+x"(Left), "+m"(State)
                       : "x"(Right)
                       : "memory");
    std::memcpy(&Bits, &Left, 8);
  } else {
    float Left, Right;
    const uint32_t A32 = uint32_t(A), B32 = uint32_t(B);
    std::memcpy(&Left, &A32, 4);
    std::memcpy(&Right, &B32, 4);
    if (Maximum)
      __asm__ volatile("ldmxcsr %1\n\tmaxss %2,%0\n\tstmxcsr %1"
                       : "+x"(Left), "+m"(State)
                       : "x"(Right)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tminss %2,%0\n\tstmxcsr %1"
                       : "+x"(Left), "+m"(State)
                       : "x"(Right)
                       : "memory");
    uint32_t Result;
    std::memcpy(&Result, &Left, 4);
    Bits = Result;
  }
  _mm_setcsr(Saved);
  return Bits;
}
#endif

TEST(X86FPMinMaxContract, NativeSourceSelectionPayloadAndStateMatrix) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  const std::array<uint64_t, 18> Single = {
      0,          0x80000000, 1,          0x80000001, 0x007fffff, 0x807fffff,
      0x00800000, 0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
      0x7f800000, 0xff800000, 0x7fc12345, 0xffc54321, 0x7f812345, 0xff854321};
  const std::array<uint64_t, 18> DoubleValues = {0,
                                                 0x8000000000000000,
                                                 1,
                                                 0x8000000000000001,
                                                 0x000fffffffffffff,
                                                 0x800fffffffffffff,
                                                 0x0010000000000000,
                                                 0x8010000000000000,
                                                 0x3ff0000000000000,
                                                 0xbff0000000000000,
                                                 0x7fefffffffffffff,
                                                 0xffefffffffffffff,
                                                 0x7ff0000000000000,
                                                 0xfff0000000000000,
                                                 0x7ff8123456789abc,
                                                 0xfff8abcdef123456,
                                                 0x7ff0123456789abc,
                                                 0xfff0abcdef123456};
  for (bool Double : {false, true})
    for (bool Maximum : {false, true})
      for (unsigned Rounding = 0; Rounding < 4; ++Rounding)
        for (unsigned Environment = 0; Environment < 4; ++Environment)
          for (bool Sticky : {false, true})
            for (bool UnderflowMasked : {false, true}) {
              const uint32_t Incoming =
                  (0x1f80U & ~(UnderflowMasked ? 0 : 0x800U)) |
                  (Rounding << 13) | ((Environment & 1) ? 0x40U : 0) |
                  ((Environment & 2) ? 0x8000U : 0) | (Sticky ? 0x25U : 0);
              const auto &Values = Double ? DoubleValues : Single;
              const unsigned Element = Double ? 8 : 4;
              for (auto A : Values)
                for (auto B : Values) {
                  uint32_t ExpectedState = Incoming;
                  const uint64_t ExpectedNumber =
                      nativeMinMax(A, B, Double, Maximum, ExpectedState);
                  NdOpEmulator Emulator(Image);
                  Emulator.setStrictMode(true);
                  Emulator.setMXCSR(Incoming);
                  Emulator.setRegisterBytes(10001, repeated(A, Element, 16));
                  Emulator.setRegisterBytes(10002, repeated(B, Element, 16));
                  ASSERT_TRUE(
                      Emulator.step(minMaxOp(Maximum, Double, true, 16, 1)));
                  auto Expected = repeated(ExpectedNumber, Element, 16);
                  std::fill(Expected.begin() + Element, Expected.end(), 0);
                  ASSERT_EQ(Emulator.getRegisterBytes(10000), Expected)
                      << "A=" << A << " B=" << B << " double=" << Double
                      << " max=" << Maximum << " csr=" << Incoming;
                  ASSERT_EQ(Emulator.getMXCSR(), ExpectedState)
                      << "A=" << A << " B=" << B << " double=" << Double
                      << " max=" << Maximum << " csr=" << Incoming;
                }
            }
#else
  GTEST_SKIP() << "native x64 compiler with inline assembly required";
#endif
}

TEST(X86FPMinMaxContract, MasksAndSAEKeepInactiveExceptionsOutOfCompletion) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Maximum : {false, true})
    for (bool SAE : {false, true})
      for (unsigned Bytes : {16U, 32U, 64U})
        for (uint64_t Mask : {UINT64_C(0), UINT64_C(1), UINT64_C(2)}) {
          const uint32_t Incoming = 0x9f80;
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setMXCSR(Incoming);
          auto Left = repeated(0x3f800000, 4, Bytes);
          auto Right = repeated(0x7f812345, 4, Bytes);
          Right[0] = 1;
          Right[1] = Right[2] = Right[3] = 0;
          Emulator.setRegisterBytes(10001, Left);
          Emulator.setRegisterBytes(10002, Right);
          ASSERT_TRUE(
              Emulator.step(minMaxOp(Maximum, false, false, Bytes, Mask, SAE)));
          std::vector<uint8_t> Expected(Bytes, 0);
          if (Mask & 1) {
            const auto First = repeated(Maximum ? 0x3f800000 : 1, 4, 4);
            std::copy(First.begin(), First.end(), Expected.begin());
          }
          if (Mask & 2)
            std::copy(Right.begin() + 4, Right.begin() + 8,
                      Expected.begin() + 4);
          EXPECT_EQ(Emulator.getRegisterBytes(10000), Expected);
          EXPECT_EQ(
              Emulator.getMXCSR(),
              Incoming |
                  (SAE ? 0U : (Mask & 1 ? 2U : 0U) | (Mask & 2 ? 1U : 0U)));
        }
}

TEST(X86FPMinMaxContract, MixedPackedLanesMatchNativeSelectedBitsAndFlags) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Double : {false, true})
    for (bool Maximum : {false, true})
      for (unsigned Bytes : {16U, 32U, 64U})
        for (unsigned Environment = 0; Environment < 4; ++Environment)
          for (bool SAE : {false, true}) {
            const unsigned Element = Double ? 8 : 4;
            const unsigned Lanes = Bytes / Element;
            const uint64_t Sign =
                Double ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
            const uint64_t QNaN =
                Double ? UINT64_C(0x7ff8123456789abc) : UINT64_C(0x7fc12345);
            const uint64_t SNaN =
                Double ? UINT64_C(0xfff0123456789abc) : UINT64_C(0xff812345);
            for (uint64_t Mask : {UINT64_C(0), UINT64_C(1), UINT64_C(0xaaaa),
                                  (UINT64_C(1) << Lanes) - 1}) {
              const uint32_t Incoming = 0x1f80 | 0x25 |
                                        ((Environment & 1) ? 0x40U : 0) |
                                        ((Environment & 2) ? 0x8000U : 0);
              uint32_t ExpectedState = Incoming;
              std::vector<uint8_t> Left(Bytes), Right(Bytes),
                  Expected(Bytes, 0);
              for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
                const uint64_t A = Lane & 1 ? QNaN : Sign;
                const uint64_t B = Lane & 2 ? SNaN : 1;
                const auto ABytes = repeated(A, Element, Element);
                const auto BBytes = repeated(B, Element, Element);
                std::copy(ABytes.begin(), ABytes.end(),
                          Left.begin() + Lane * Element);
                std::copy(BBytes.begin(), BBytes.end(),
                          Right.begin() + Lane * Element);
                if ((Mask >> Lane) & 1) {
                  const auto Result = repeated(
                      nativeMinMax(A, B, Double, Maximum, ExpectedState),
                      Element, Element);
                  std::copy(Result.begin(), Result.end(),
                            Expected.begin() + Lane * Element);
                }
              }
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              Emulator.setMXCSR(Incoming);
              Emulator.setRegisterBytes(10001, Left);
              Emulator.setRegisterBytes(10002, Right);
              ASSERT_TRUE(Emulator.step(
                  minMaxOp(Maximum, Double, false, Bytes, Mask, SAE)));
              EXPECT_EQ(Emulator.getRegisterBytes(10000), Expected);
              EXPECT_EQ(Emulator.getMXCSR(), SAE ? Incoming : ExpectedState);
            }
          }
#else
  GTEST_SKIP() << "native x64 compiler with inline assembly required";
#endif
}

TEST(X86FPMinMaxContract, ArithmeticUnderflowAndFTZStillApplyToMultiplication) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setMXCSR(0x9f80);
  Emulator.setRegisterBytes(10001, repeated(0x00800000, 4, 16));
  Emulator.setRegisterBytes(10002, repeated(0x3f000000, 4, 16));
  auto Op = minMaxOp(false, false, false, 16, 15);
  Op.Inputs[1] =
      NdVar::cst(makeX86FPArithControl(X86FPArithKind::Multiply, false, false,
                                       false, X86FPRounding::MXCSR),
                 2);
  ASSERT_TRUE(Emulator.step(Op));
  EXPECT_EQ(Emulator.getRegisterBytes(10000), std::vector<uint8_t>(16, 0));
  EXPECT_EQ(Emulator.getMXCSR(), 0x9fb0U);
}

TEST(X86FPMinMaxContract, UnorderedSelectionStillAppliesDAZToSecondSource) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Double : {false, true})
    for (bool Maximum : {false, true})
      for (bool Negative : {false, true})
        for (bool DAZ : {false, true}) {
          const unsigned Element = Double ? 8 : 4;
          const uint64_t Sign = Negative
                                    ? (Double ? UINT64_C(0x8000000000000000)
                                              : UINT64_C(0x80000000))
                                    : 0;
          const uint64_t QNaN =
              Double ? UINT64_C(0x7ff8123456789abc) : UINT64_C(0x7fc12345);
          const uint32_t Incoming = 0x9f80U | (DAZ ? 0x40U : 0);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setMXCSR(Incoming);
          Emulator.setRegisterBytes(10001, repeated(QNaN, Element, 16));
          Emulator.setRegisterBytes(10002, repeated(Sign | 1, Element, 16));
          ASSERT_TRUE(Emulator.step(minMaxOp(Maximum, Double, true, 16, 1)));
          auto Expected = repeated(Sign | (DAZ ? 0 : 1), Element, 16);
          std::fill(Expected.begin() + Element, Expected.end(), 0);
          EXPECT_EQ(Emulator.getRegisterBytes(10000), Expected);
          EXPECT_EQ(Emulator.getMXCSR(), Incoming | 1U);
        }
}

TEST(X86FPMinMaxContract, UnmaskedDenormalCommitsOnlyExceptionState) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (bool Maximum : {false, true}) {
    NdOpEmulator Emulator(Image);
    const uint32_t Incoming = 0x9f80 & ~0x100U;
    const std::vector<uint8_t> Sentinel(16, 0x59);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(Incoming);
    Emulator.setRegisterBytes(10001, repeated(0x3f800000, 4, 16));
    Emulator.setRegisterBytes(10002, repeated(1, 4, 16));
    Emulator.setRegisterBytes(10000, Sentinel);
    EXPECT_FALSE(Emulator.step(minMaxOp(Maximum, false, false, 16, 15)));
    EXPECT_EQ(Emulator.getRegisterBytes(10000), Sentinel);
    EXPECT_EQ(Emulator.getMXCSR(), Incoming | 2U);
  }
}
} // namespace

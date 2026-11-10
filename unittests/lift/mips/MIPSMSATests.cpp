//===- MIPSMSATests.cpp - Independent MSA word-lane semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check complete vectors, overlapping operands, FPR low views and enabled
/// P5600 execution without using production encoding/selector tables.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/ErrorHandling.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <algorithm>
#include <array>
#include <ios>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace neverd;

namespace {
BinaryImage msaImage(Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Bitness::Bits32;
  Image.Entry = 0x10000;
  Image.MIPS.emplace();
  Image.MIPS->ISARevision = 5;
  Image.MIPS->HasMSA = true;
  Image.MIPS->FPABI = 6;
  return Image;
}

std::vector<uint8_t> msaBytes(llvm::ArrayRef<uint32_t> Words, bool Little) {
  std::vector<uint8_t> Bytes;
  for (uint32_t Word : Words)
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(Word >> (8 * (Little ? I : 3 - I)));
  return Bytes;
}

std::vector<LowOp> msaLift(Decoder &Decode, Arch Target, uint32_t Word) {
  const auto Bytes = msaBytes({Word}, archLittleEndian(Target));
  DecodedInsn Instruction;
  EXPECT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000, Instruction),
      4);
  std::vector<LowOp> Ops;
  Decode.liftToLow(Instruction, Ops);
  return Ops;
}

bool msaExecute(Decoder &Decode, Arch Target, uint32_t Word,
                NdOpEmulator &Emulator) {
  for (const auto &Op : msaLift(Decode, Target, Word))
    if (!Emulator.step(Op))
      return false;
  return true;
}

enum class VectorOperation {
  Add,
  Sub,
  And,
  Or,
  Nor,
  Xor,
  ShiftLeft,
  ShiftRightSigned,
  ShiftRight,
  ClearBit,
  SetBit,
  NegateBit,
  Multiply,
  MultiplyAdd,
  MultiplySub,
  Equal,
  SignedLess,
  UnsignedLess,
  SignedLessEqual,
  UnsignedLessEqual,
  SignedMaximum,
  UnsignedMaximum,
  SignedMinimum,
  UnsignedMinimum,
  AbsoluteMaximum,
  AbsoluteMinimum,
  AddAbsolute,
  AddSaturatedAbsolute,
  AddSaturatedSigned,
  AddSaturatedUnsigned,
  SubSaturatedSigned,
  SubSaturatedUnsigned,
  AverageSigned,
  AverageUnsigned,
  RoundAverageSigned,
  RoundAverageUnsigned
};
struct VectorCase {
  uint32_t Encoding;
  unsigned ElementBytes;
  VectorOperation Operation;
};
constexpr VectorCase VectorCases[] = {
#define NEVERD_MIPS_MSA_VECTOR(Encoding, Bytes, Operation)                     \
  {Encoding, Bytes, VectorOperation::Operation},
#include "MIPSMSAVectorFixtures.def"
};
constexpr std::array<uint32_t, 4> VectorInputs[][2] = {
    {{0x80017fff, 0x80000000, 1, 0},
     {0x80ff7f01, 0x7fffffff, 0xffffffff, 0xffffffff}},
    {{0x80000000, 0, 0, 0x80000000},
     {0x80000000, 0xffffffff, 0xffffffff, 0x7fffffff}},
    {{0xffffffff, 0x7fffffff, 1, 0x80000000}, {63, 0xffffffff, 64, 0x80000000}},
    {{0x80000000, 0x80000000, 0xffffffff, 0x7fffffff},
     {65, 0x12345678, 127, 0xffffffff}},
    {{0x80008080, 0x7fff7fff, 0x80000000, 0xffffffff},
     {0x08080707, 0x10100f0f, 32, 31}},
    {{0xffffffff, 0xffffffff, 0, 0}, {1, 0, 1, 0}},
    {{0x00ff00ff, 0x0000ffff, 0x7fffffff, 0x80000000},
     {0x01010101, 0x00010001, 0x87654321, 0x7fffffff}},
    {{0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00},
     {0xf0e0d0c0, 0xb0a09080, 0x70605040, 0x30201001}},
};

std::vector<uint8_t> vectorExpected(const VectorCase &Case,
                                    llvm::ArrayRef<uint8_t> A,
                                    llvm::ArrayRef<uint8_t> B,
                                    llvm::ArrayRef<uint8_t> D) {
  // Byte-wise carry/borrow is independent of the lifter's sized lane ops.
  std::vector<uint8_t> Result(16);
  if (Case.Operation >= VectorOperation::AddAbsolute) {
    const unsigned Bits = Case.ElementBytes * 8;
    for (unsigned Base = 0; Base < 16; Base += Case.ElementBytes) {
      const auto Read = [&](llvm::ArrayRef<uint8_t> V) {
        uint64_t N = 0;
        for (unsigned Byte = 0; Byte < Case.ElementBytes; ++Byte)
          N |= uint64_t(V[Base + Byte]) << (Byte * 8);
        return llvm::APInt(Bits, N);
      };
      const auto UA = Read(A).zext(128), UB = Read(B).zext(128);
      const auto SA = Read(A).sext(128), SB = Read(B).sext(128);
      const auto Min = llvm::APInt::getSignedMinValue(Bits).sext(128);
      const auto Max = llvm::APInt::getSignedMaxValue(Bits).zext(128);
      const auto UMax = llvm::APInt::getMaxValue(Bits).zext(128);
      llvm::APInt V(128, 0);
      switch (Case.Operation) {
      case VectorOperation::AddAbsolute:
      case VectorOperation::AddSaturatedAbsolute:
        V = SA.abs() + SB.abs();
        if (Case.Operation == VectorOperation::AddSaturatedAbsolute &&
            V.sgt(Max))
          V = Max;
        break;
      case VectorOperation::AddSaturatedSigned:
      case VectorOperation::SubSaturatedSigned:
        V = Case.Operation == VectorOperation::AddSaturatedSigned ? SA + SB
                                                                  : SA - SB;
        if (V.slt(Min))
          V = Min;
        if (V.sgt(Max))
          V = Max;
        break;
      case VectorOperation::AddSaturatedUnsigned:
      case VectorOperation::SubSaturatedUnsigned:
        V = Case.Operation == VectorOperation::AddSaturatedUnsigned ? UA + UB
                                                                    : UA - UB;
        if (V.isNegative())
          V = llvm::APInt(128, 0);
        if (V.ugt(UMax))
          V = UMax;
        break;
      default: {
        const bool Signed =
            Case.Operation == VectorOperation::AverageSigned ||
            Case.Operation == VectorOperation::RoundAverageSigned;
        const bool Round =
            Case.Operation == VectorOperation::RoundAverageSigned ||
            Case.Operation == VectorOperation::RoundAverageUnsigned;
        V = (Signed ? SA + SB : UA + UB) + llvm::APInt(128, Round);
        V = V.ashr(1);
        break;
      }
      }
      const uint64_t N = V.trunc(Bits).getZExtValue();
      for (unsigned Byte = 0; Byte < Case.ElementBytes; ++Byte)
        Result[Base + Byte] = N >> (Byte * 8);
    }
    return Result;
  }
  if (Case.Operation == VectorOperation::AbsoluteMaximum ||
      Case.Operation == VectorOperation::AbsoluteMinimum) {
    for (unsigned Base = 0; Base != 16; Base += Case.ElementBytes) {
      const auto Magnitude = [&](llvm::ArrayRef<uint8_t> V) {
        std::vector<uint8_t> M(V.begin() + Base,
                               V.begin() + Base + Case.ElementBytes);
        if (M.back() & 128) {
          unsigned Carry = 1;
          for (auto &Byte : M) {
            unsigned N = unsigned(uint8_t(~Byte)) + Carry;
            Byte = N;
            Carry = N >> 8;
          }
        }
        return M;
      };
      auto X = Magnitude(A), Y = Magnitude(B);
      int Order = 0;
      for (unsigned I = Case.ElementBytes; I && !Order; --I)
        Order = X[I - 1] < Y[I - 1] ? -1 : X[I - 1] > Y[I - 1] ? 1 : 0;
      const auto Source =
          (Case.Operation == VectorOperation::AbsoluteMaximum ? Order > 0
                                                              : Order < 0)
              ? A
              : B;
      std::copy_n(Source.begin() + Base, Case.ElementBytes,
                  Result.begin() + Base);
    }
    return Result;
  }
  if (Case.Operation >= VectorOperation::Equal) {
    const bool Signed = Case.Operation == VectorOperation::SignedLess ||
                        Case.Operation == VectorOperation::SignedLessEqual ||
                        Case.Operation == VectorOperation::SignedMaximum ||
                        Case.Operation == VectorOperation::SignedMinimum;
    const bool Inclusive = Case.Operation == VectorOperation::SignedLessEqual ||
                           Case.Operation == VectorOperation::UnsignedLessEqual;
    // Compare bytes from most significant to least significant. Biasing only
    // the top byte handles signed 64-bit extremes without host signed overflow.
    for (unsigned Base = 0; Base != 16; Base += Case.ElementBytes) {
      int Order = 0;
      for (unsigned J = Case.ElementBytes; J != 0 && !Order; --J) {
        const unsigned Bias = Signed && J == Case.ElementBytes ? 128 : 0;
        const unsigned X = A[Base + J - 1] ^ Bias;
        const unsigned Y = B[Base + J - 1] ^ Bias;
        Order = X < Y ? -1 : X > Y ? 1 : 0;
      }
      if (Case.Operation >= VectorOperation::SignedMaximum) {
        const bool Maximum = Case.Operation == VectorOperation::SignedMaximum ||
                             Case.Operation == VectorOperation::UnsignedMaximum;
        const auto Source = (Maximum ? Order > 0 : Order < 0) ? A : B;
        std::copy_n(Source.begin() + Base, Case.ElementBytes,
                    Result.begin() + Base);
        continue;
      }
      const bool True = Case.Operation == VectorOperation::Equal ? Order == 0
                        : Inclusive                              ? Order <= 0
                                                                 : Order < 0;
      std::fill_n(Result.begin() + Base, Case.ElementBytes, True ? 255 : 0);
    }
    return Result;
  }
  if (Case.Operation >= VectorOperation::ShiftLeft &&
      Case.Operation <= VectorOperation::MultiplySub) {
    const unsigned Bits = Case.ElementBytes * 8;
    for (unsigned Base = 0; Base != 16; Base += Case.ElementBytes) {
      const unsigned Count = B[Base] & (Bits - 1);
      if (Case.Operation == VectorOperation::Multiply ||
          Case.Operation == VectorOperation::MultiplyAdd ||
          Case.Operation == VectorOperation::MultiplySub) {
        unsigned Carry = 0;
        for (unsigned Byte = 0; Byte != Case.ElementBytes; ++Byte) {
          unsigned Sum = Carry;
          for (unsigned J = 0; J <= Byte; ++J)
            Sum += unsigned(A[Base + J]) * B[Base + Byte - J];
          Result[Base + Byte] = uint8_t(Sum);
          Carry = Sum >> 8;
        }
        if (Case.Operation != VectorOperation::Multiply) {
          int Carry = 0;
          for (unsigned I = 0; I < Case.ElementBytes; ++I) {
            const int N = Case.Operation == VectorOperation::MultiplyAdd
                              ? D[Base + I] + Result[Base + I] + Carry
                              : D[Base + I] - Result[Base + I] - Carry;
            Result[Base + I] = uint8_t(N);
            Carry =
                Case.Operation == VectorOperation::MultiplyAdd ? N >> 8 : N < 0;
          }
        }
        continue;
      }
      for (unsigned Bit = 0; Bit != Bits; ++Bit) {
        const auto Get = [&](unsigned N) {
          return (A[Base + N / 8] >> (N % 8)) & 1;
        };
        unsigned Value = Get(Bit);
        if (Case.Operation == VectorOperation::ShiftLeft)
          Value = Bit < Count ? 0 : Get(Bit - Count);
        else if (Case.Operation == VectorOperation::ShiftRight ||
                 Case.Operation == VectorOperation::ShiftRightSigned)
          Value = Bit + Count < Bits ? Get(Bit + Count)
                  : Case.Operation == VectorOperation::ShiftRightSigned
                      ? Get(Bits - 1)
                      : 0;
        else if (Bit == Count)
          Value = Case.Operation == VectorOperation::ClearBit ? 0
                  : Case.Operation == VectorOperation::SetBit ? 1
                                                              : !Value;
        Result[Base + Bit / 8] |= Value << (Bit % 8);
      }
    }
    return Result;
  }
  int Carry = 0;
  for (unsigned I = 0; I != 16; ++I) {
    if (I % Case.ElementBytes == 0)
      Carry = 0;
    switch (Case.Operation) {
    case VectorOperation::Add: {
      const unsigned Value = A[I] + B[I] + Carry;
      Result[I] = uint8_t(Value);
      Carry = Value >> 8;
      break;
    }
    case VectorOperation::Sub: {
      const int Value = A[I] - B[I] - Carry;
      Result[I] = uint8_t(Value);
      Carry = Value < 0;
      break;
    }
    case VectorOperation::And:
      Result[I] = A[I] & B[I];
      break;
    case VectorOperation::Or:
      Result[I] = A[I] | B[I];
      break;
    case VectorOperation::Nor:
      Result[I] = uint8_t(~(A[I] | B[I]));
      break;
    case VectorOperation::Xor:
      Result[I] = A[I] ^ B[I];
      break;
    default:
      llvm_unreachable("comparison handled before byte arithmetic");
    }
  }
  return Result;
}

class MIPSMSA : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSMSA, FillInsertCopyEveryWordAndSharedLowViews) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const auto &TRI = getTargetRegInfo(GetParam());
  for (unsigned Reg = 0; Reg != 32; ++Reg) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, 0x12345678);
    ASSERT_TRUE(
        msaExecute(Decode, GetParam(), 0x7b02201e | (Reg << 6), Emulator));
    std::array<uint32_t, 4> Expected;
    Expected.fill(0x12345678);
    for (unsigned Lane = 0; Lane != 4; ++Lane) {
      Emulator.setRegister(mipsreg::A1, 0x80000000u | Lane);
      ASSERT_TRUE(msaExecute(Decode, GetParam(),
                             0x79302819 | (Lane << 16) | (Reg << 6), Emulator));
      Expected[Lane] = 0x80000000u | Lane;
      EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(Reg)),
                msaBytes(Expected, true));
      ASSERT_TRUE(msaExecute(Decode, GetParam(),
                             0x78b00099 | (Lane << 16) | (Reg << 11),
                             Emulator));
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected[Lane]);
      EXPECT_EQ(Emulator.getRegister(mipsreg::w(Reg)),
                uint64_t(Expected[0]) | (uint64_t(Expected[1]) << 32));
    }
    EXPECT_EQ(TRI.findWideReg(mipsreg::w(Reg), 8),
              std::make_pair(mipsreg::w(Reg), uint16_t(16)));
    EXPECT_EQ(TRI.maxRegisterWidth(mipsreg::w(Reg)), 16);
    EXPECT_FALSE(TRI.writeZeroExtends(mipsreg::w(Reg), 8));
  }
}

TEST_P(MIPSMSA, AdditionWrapsPerLaneAndCapturesAliasedSources) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const std::array<uint32_t, 4> A = {0xffffffff, 0x7fffffff, 0x80000000,
                                     0x12345678};
  const std::array<uint32_t, 4> B = {1, 1, 0x80000000, 0x87654321};
  const std::array<uint32_t, 4> Expected = {0, 0x80000000, 0, 0x99999999};
  for (unsigned Destination : {0u, 1u, 2u, 31u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(mipsreg::w(0), msaBytes(A, true));
    Emulator.setRegisterBytes(mipsreg::w(1), msaBytes(B, true));
    ASSERT_TRUE(msaExecute(Decode, GetParam(), 0x7841000e | (Destination << 6),
                           Emulator));
    EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(Destination)),
              msaBytes(Expected, true));
  }
}

TEST_P(MIPSMSA, IntegerFormatsPreserveLaneBoundariesAndAliasedInputs) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (const auto &Case : VectorCases)
    for (const auto &Input : VectorInputs)
      for (unsigned Destination : {0u, 1u, 2u, 31u})
        for (bool SameSource : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << std::hex << Case.Encoding << ": wd=" << Destination
                       << " same=" << SameSource);
          const auto A = msaBytes(Input[0], true);
          const auto B = msaBytes(Input[1], true);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegisterBytes(mipsreg::w(0), A);
          Emulator.setRegisterBytes(mipsreg::w(1), B);
          if (Destination > 1)
            Emulator.setRegisterBytes(mipsreg::w(Destination),
                                      std::vector<uint8_t>(16, 0x5a));
          const uint32_t Word =
              Case.Encoding | (Destination << 6) | (SameSource ? 0 : (1 << 16));
          ASSERT_TRUE(msaExecute(Decode, GetParam(), Word, Emulator));
          EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(Destination)),
                    vectorExpected(Case, A, SameSource ? A : B,
                                   Destination == 0 ? A
                                   : Destination == 1
                                       ? B
                                       : std::vector<uint8_t>(16, 0x5a)));
          if (Destination != 0)
            EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(0)), A);
          if (Destination != 1)
            EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(1)), B);
        }
}

TEST_P(MIPSMSA, IntegerFormatsRefuseMissingWholeOrUpperVectorState) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (const auto &Case : VectorCases)
    for (bool Strict : {false, true})
      for (unsigned Missing : {0u, 1u})
        for (unsigned Available : {0u, 8u}) {
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(Strict);
          Emulator.setRegisterBytes(mipsreg::w(1 - Missing),
                                    std::vector<uint8_t>(16, 0xff));
          if (Available)
            Emulator.setRegister(mipsreg::w(Missing), 0x123456789abcdef0);
          const std::vector<uint8_t> Before(16, 0x5a);
          Emulator.setRegisterBytes(mipsreg::w(2), Before);
          EXPECT_FALSE(msaExecute(Decode, GetParam(),
                                  Case.Encoding | (1 << 16) | (2 << 6),
                                  Emulator));
          EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(2)), Before);
          EXPECT_FALSE(Emulator.skips().any());
        }
}

TEST_P(MIPSMSA, MultiplyAccumulateRequiresEveryByteOfTheOldDestination) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (uint32_t Word : {0x78810092u, 0x79010092u})
    for (unsigned Available : {0u, 8u}) {
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegisterBytes(mipsreg::w(0), std::vector<uint8_t>(16, 1));
      E.setRegisterBytes(mipsreg::w(1), std::vector<uint8_t>(16, 2));
      if (Available)
        E.setRegister(mipsreg::w(2), 0x123456789abcdef0);
      const auto Before = E.getRegisterBytes(mipsreg::w(2));
      EXPECT_FALSE(msaExecute(Decode, GetParam(), Word, E));
      EXPECT_EQ(E.getRegisterBytes(mipsreg::w(2)), Before);
      if (Available)
        EXPECT_EQ(E.getRegister(mipsreg::w(2)), UINT64_C(0x123456789abcdef0));
    }
}

TEST_P(MIPSMSA, ZeroGPRAndMissingVectorStateStayDistinct) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  EXPECT_FALSE(msaExecute(Decode, GetParam(), 0x78b00099, Emulator));
  EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
  EXPECT_TRUE(msaExecute(Decode, GetParam(), 0x78b00019, Emulator));
  Emulator.setRegister(mipsreg::A0, 3);
  EXPECT_FALSE(msaExecute(Decode, GetParam(), 0x79302019, Emulator));
  EXPECT_FALSE(Emulator.getRegisterBytes(mipsreg::w(0)));
  ASSERT_TRUE(msaExecute(Decode, GetParam(), 0x7b02001e, Emulator));
  EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(0)),
            std::vector<uint8_t>(16, 0));
}

TEST_P(MIPSMSA, ReusedDecoderConsumesRevisionAndFeatureEvidence) {
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (bool Enabled : {true, false, true, false}) {
    Image.MIPS->ISARevision = Enabled ? 5 : 2;
    Image.MIPS->HasMSA = Enabled;
    ASSERT_TRUE(Decode.selectMode(Image, Image.Entry));
    if (Enabled)
      EXPECT_NO_THROW(msaLift(Decode, GetParam(), 0x7b02201e));
    else
      EXPECT_THROW(msaLift(Decode, GetParam(), 0x7b02201e),
                   UnliftedInstruction);
  }
  Image.MIPS->ISARevision = 5;
  for (bool Enabled : {true, false, true, false}) {
    Image.MIPS->HasMSA = Enabled;
    ASSERT_TRUE(Decode.selectMode(Image, Image.Entry));
    if (Enabled)
      EXPECT_NO_THROW(msaLift(Decode, GetParam(), 0x7b02201e));
    else
      EXPECT_THROW(msaLift(Decode, GetParam(), 0x7b02201e),
                   UnliftedInstruction);
  }
  ASSERT_TRUE(Decode.init(GetParam()));
  EXPECT_THROW(msaLift(Decode, GetParam(), 0x7b02201e), UnliftedInstruction);
  Image.MIPS->ISARevision = 4;
  EXPECT_FALSE(Decode.init(Image));
}

TEST_P(MIPSMSA, UnadmittedFormatsRefuseBeforeEmittingEffects) {
  MIPSLifter Lifter(GetParam());
  Lifter.setMSAEnabled(true);
  for (uint32_t Word : {0x7b03201eu, 0x79342819u, 0x78f00099u, 0x7a800012u}) {
    cs_insn Instruction{};
    Instruction.address = 0x10000;
    Instruction.size = 4;
    const auto Bytes = msaBytes({Word}, archLittleEndian(GetParam()));
    std::copy(Bytes.begin(), Bytes.end(), Instruction.bytes);
    std::vector<LowOp> Ops(1);
    EXPECT_THROW(Lifter.lift(Instruction, Ops), UnliftedInstruction);
    EXPECT_EQ(Ops.size(), 1u);
  }
}

TEST_P(MIPSMSA, LiteralLanesAndFPRViewsMatchEnabledP5600) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "pinned Unicorn MIPS reference unavailable";
#else
  const uint32_t Setup[] = {0x40088005, 0x3c090800, 0x01094025, 0x40888005,
                            0x000000c0, 0x40086000, 0x3c092400, 0x01094025,
                            0x40886000, 0x000000c0};
  const uint32_t Words[] = {0x7b02201e, 0x79312819, 0x7840004e, 0x78b10899};
  auto Image = msaImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (const auto &Input : {std::array<uint32_t, 2>{0xffffffff, 0x7fffffff},
                            std::array<uint32_t, 2>{0x11223344, 0x80000000}}) {
    uc_engine *CPU = nullptr;
    ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                      uc_mode(UC_MODE_MIPS32 | (archLittleEndian(GetParam())
                                                    ? 0
                                                    : UC_MODE_BIG_ENDIAN)),
                      &CPU),
              UC_ERR_OK);
    const auto Close = [](uc_engine *P) { uc_close(P); };
    std::unique_ptr<uc_engine, decltype(Close)> Owner(CPU, Close);
    ASSERT_EQ(uc_ctl_set_cpu_model(CPU, UC_CPU_MIPS32_P5600), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(CPU, 0x10000, 4096, UC_PROT_ALL), UC_ERR_OK);
    std::vector<uint32_t> Program(std::begin(Setup), std::end(Setup));
    Program.insert(Program.end(), std::begin(Words), std::end(Words));
    Program.insert(Program.end(), {0x44030800, 0x44660800}); // F1 low/high
    const auto Bytes = msaBytes(Program, archLittleEndian(GetParam()));
    ASSERT_EQ(uc_mem_write(CPU, 0x10000, Bytes.data(), Bytes.size()),
              UC_ERR_OK);
    uint64_t A = Input[0], B = Input[1];
    ASSERT_EQ(uc_reg_write(CPU, UC_MIPS_REG_A0, &A), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(CPU, UC_MIPS_REG_A1, &B), UC_ERR_OK);
    ASSERT_EQ(
        uc_emu_start(CPU, 0x10000, 0x10000 + Bytes.size(), 0, Program.size()),
        UC_ERR_OK);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, A);
    Emulator.setRegister(mipsreg::A1, B);
    for (uint32_t Word : Words)
      ASSERT_TRUE(msaExecute(Decode, GetParam(), Word, Emulator));
    uint64_t Scalar = 0, Low = 0, High = 0;
    ASSERT_EQ(uc_reg_read(CPU, UC_MIPS_REG_V0, &Scalar), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(CPU, UC_MIPS_REG_V1, &Low), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(CPU, UC_MIPS_REG_A2, &High), UC_ERR_OK);
    EXPECT_EQ(uint32_t(Scalar), uint32_t(B + B));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), uint32_t(Scalar));
    EXPECT_EQ(uint32_t(Low), uint32_t(A + A));
    EXPECT_EQ(uint32_t(High), uint32_t(B + B));
    EXPECT_EQ(Emulator.getRegister(mipsreg::w(1)),
              uint64_t(uint32_t(Low)) | uint64_t(uint32_t(High)) << 32);
  }
#endif
}

TEST_P(MIPSMSA, IntegerFormatsMatchEnabledP5600AcrossAllOutputBytes) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "pinned Unicorn MIPS reference unavailable";
#else
  const uint32_t Setup[] = {0x40088005, 0x3c090800, 0x01094025, 0x40888005,
                            0x000000c0, 0x40086000, 0x3c092400, 0x01094025,
                            0x40886000, 0x000000c0};
  for (const auto &Case : VectorCases)
    for (const auto &Input : VectorInputs)
      for (unsigned Destination : {0u, 1u, 2u, 31u})
        for (bool SameSource : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << std::hex << Case.Encoding << ": wd=" << Destination
                       << " same=" << SameSource);
          uc_engine *CPU = nullptr;
          ASSERT_EQ(
              uc_open(UC_ARCH_MIPS,
                      uc_mode(UC_MODE_MIPS32 | (archLittleEndian(GetParam())
                                                    ? 0
                                                    : UC_MODE_BIG_ENDIAN)),
                      &CPU),
              UC_ERR_OK);
          const auto Close = [](uc_engine *P) { uc_close(P); };
          std::unique_ptr<uc_engine, decltype(Close)> Owner(CPU, Close);
          ASSERT_EQ(uc_ctl_set_cpu_model(CPU, UC_CPU_MIPS32_P5600), UC_ERR_OK);
          ASSERT_EQ(uc_mem_map(CPU, 0x10000, 4096, UC_PROT_ALL), UC_ERR_OK);
          std::vector<uint32_t> Program(std::begin(Setup), std::end(Setup));
          for (unsigned Vector = 0; Vector != 2; ++Vector) {
            Program.push_back(0x7b02001e | ((Vector ? 20 : 4) << 11) |
                              (Vector << 6));
            for (unsigned Lane = 0; Lane != 4; ++Lane) {
              const unsigned Register = (Vector ? 20 : 4) + Lane;
              Program.push_back(0x79300019 | (Lane << 16) | (Register << 11) |
                                (Vector << 6));
              uint64_t Value = Input[Vector][Lane];
              ASSERT_EQ(uc_reg_write(CPU, UC_MIPS_REG_0 + Register, &Value),
                        UC_ERR_OK);
            }
          }
          if (Destination > 1) {
            uint64_t Initial = 0x5a5a5a5a;
            ASSERT_EQ(uc_reg_write(CPU, UC_MIPS_REG_T2, &Initial), UC_ERR_OK);
            Program.push_back(0x7b02501e | (Destination << 6));
          }
          Program.push_back(Case.Encoding | (Destination << 6) |
                            (SameSource ? 0 : (1 << 16)));
          for (unsigned Lane = 0; Lane != 4; ++Lane)
            Program.push_back(0x78b00019 | (Lane << 16) | (Destination << 11) |
                              ((16 + Lane) << 6));
          const auto Bytes = msaBytes(Program, archLittleEndian(GetParam()));
          ASSERT_EQ(uc_mem_write(CPU, 0x10000, Bytes.data(), Bytes.size()),
                    UC_ERR_OK);
          ASSERT_EQ(uc_emu_start(CPU, 0x10000, 0x10000 + Bytes.size(), 0,
                                 Program.size()),
                    UC_ERR_OK);
          const auto A = msaBytes(Input[0], true);
          const auto B = msaBytes(Input[1], true);
          std::vector<uint8_t> Actual;
          for (unsigned Lane = 0; Lane != 4; ++Lane) {
            uint64_t Value = 0;
            ASSERT_EQ(uc_reg_read(CPU, UC_MIPS_REG_S0 + Lane, &Value),
                      UC_ERR_OK);
            for (unsigned I = 0; I != 4; ++I)
              Actual.push_back(uint8_t(Value >> (I * 8)));
          }
          const auto &Right = SameSource ? A : B;
          const auto Expected = vectorExpected(
              Case, A, Right,
              Destination == 0   ? A
              : Destination == 1 ? B
                                 : std::vector<uint8_t>(16, 0x5a));
          for (unsigned I = 0; I < Actual.size(); ++I)
            EXPECT_EQ(Actual[I], Expected[I]) << "byte " << I;
        }
#endif
}

TEST_P(MIPSMSA, ImmediateBitFormatsExhaustEveryPrefixAndCaptureAliases) {
  auto Image = msaImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  MIPSLifter L(GetParam());
  L.setMSAEnabled(true);
  for (unsigned Operation = 0; Operation != 6; ++Operation)
    for (unsigned Field = 0; Field != 128; ++Field)
      for (unsigned Destination : {0u, 2u}) {
        const uint32_t Word =
            0x78000009 | (Operation << 23) | (Field << 16) | (Destination << 6);
        if (Field >= 120) {
          const auto Bytes = msaBytes({Word}, archLittleEndian(GetParam()));
          cs_insn Raw{};
          Raw.size = 4;
          std::copy(Bytes.begin(), Bytes.end(), Raw.bytes);
          std::vector<LowOp> Ops;
          EXPECT_THROW(L.lift(Raw, Ops), std::runtime_error);
          EXPECT_TRUE(Ops.empty());
          continue;
        }
        const unsigned Bits = Field < 64    ? 64
                              : Field < 96  ? 32
                              : Field < 112 ? 16
                                            : 8;
        const unsigned Count = Field % Bits;
        for (unsigned Pattern : {0u, 1u, 2u, 3u}) {
          std::vector<uint8_t> Input(16), Expected(16);
          for (unsigned I = 0; I != 16; ++I)
            Input[I] = Pattern == 0   ? 0
                       : Pattern == 1 ? 255
                       : Pattern == 2 ? 0x80
                                      : I * 37 + 19;
          for (unsigned I = 0; I != 16; I += Bits / 8) {
            uint64_t V = 0;
            for (unsigned B = 0; B != Bits / 8; ++B)
              V |= uint64_t(Input[I + B]) << (8 * B);
            uint64_t Result = V;
            switch (Operation) {
            case 0:
              Result = V << Count;
              break;
            case 1:
              Result = V >> Count;
              if (Count && (V & (UINT64_C(1) << (Bits - 1))))
                Result |= UINT64_MAX << (Bits - Count);
              break;
            case 2:
              Result = V >> Count;
              break;
            case 3:
              Result = V & ~(UINT64_C(1) << Count);
              break;
            case 4:
              Result = V | (UINT64_C(1) << Count);
              break;
            case 5:
              Result = V ^ (UINT64_C(1) << Count);
              break;
            }
            for (unsigned B = 0; B != Bits / 8; ++B)
              Expected[I + B] = Result >> (8 * B);
          }
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegisterBytes(mipsreg::w(0), Input);
          ASSERT_TRUE(msaExecute(D, GetParam(), Word, E));
          EXPECT_EQ(E.getRegisterBytes(mipsreg::w(Destination)), Expected)
              << "operation=" << Operation << " field=" << Field
              << " pattern=" << Pattern;
        }
      }
}

TEST_P(MIPSMSA, UnaryCountsObserveAllBitsAndDefineZeroAndFullLanes) {
  auto Image = msaImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  for (unsigned Operation = 0; Operation != 3; ++Operation)
    for (unsigned Format = 0; Format != 4; ++Format)
      for (unsigned Destination : {0u, 2u, 31u})
        for (unsigned Pattern : {0u, 1u, 2u, 3u}) {
          const unsigned Bytes = 1u << Format, Bits = Bytes * 8;
          std::vector<uint8_t> Input(16), Expected(16, 0);
          for (unsigned I = 0; I != 16; ++I)
            Input[I] = Pattern == 0   ? 0
                       : Pattern == 1 ? 255
                       : Pattern == 2 ? 0x80
                                      : I * 37 + 19;
          for (unsigned I = 0; I != 16; I += Bytes) {
            unsigned Count = 0;
            for (unsigned B = 0; B != Bits; ++B) {
              unsigned Bit = Operation == 0 ? B : Bits - B - 1;
              const bool Set = Input[I + Bit / 8] & (1u << (Bit % 8));
              if (Operation == 0)
                Count += Set;
              else if (Set == (Operation == 1))
                ++Count;
              else
                break;
            }
            Expected[I] = Count;
          }
          const uint32_t Word = 0x7b00001e | ((Operation + 1) << 18) |
                                (Format << 16) | (Destination << 6);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegisterBytes(mipsreg::w(0), Input);
          ASSERT_TRUE(msaExecute(D, GetParam(), Word, E));
          EXPECT_EQ(E.getRegisterBytes(mipsreg::w(Destination)), Expected);
        }
}

TEST_P(MIPSMSA, ImmediateLogicalBytesAndThreeInputMasksPreserveEveryBit) {
  auto Image = msaImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  const std::vector<uint8_t> A = {0,  1,  2,  0xff, 0x80, 0x7f, 0x55, 0xaa,
                                  17, 29, 31, 67,   127,  9,    123,  45};
  for (unsigned Operation = 0; Operation != 4; ++Operation)
    for (unsigned Immediate = 0; Immediate != 256; ++Immediate)
      for (unsigned Destination : {0u, 31u}) {
        auto Expected = A;
        for (auto &B : Expected)
          B = Operation == 0   ? B & Immediate
              : Operation == 1 ? B | Immediate
              : Operation == 2 ? ~(B | Immediate)
                               : B ^ Immediate;
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegisterBytes(mipsreg::w(0), A);
        ASSERT_TRUE(msaExecute(D, GetParam(),
                               0x78000000 | (Operation << 24) |
                                   (Immediate << 16) | (Destination << 6),
                               E));
        EXPECT_EQ(E.getRegisterBytes(mipsreg::w(Destination)), Expected);
      }
  const std::vector<uint8_t> B(A.rbegin(), A.rend()), C(16, 0x5a);
  for (unsigned Operation = 0; Operation != 3; ++Operation)
    for (unsigned Destination : {0u, 1u, 2u})
      for (bool SameSource : {false, true}) {
        const auto Old = Destination == 0 ? A : Destination == 1 ? B : C;
        const auto Other = SameSource ? A : B;
        auto Expected = A;
        for (unsigned I = 0; I != 16; ++I)
          Expected[I] =
              Operation == 0   ? (A[I] & Other[I]) | (Old[I] & ~Other[I])
              : Operation == 1 ? (A[I] & ~Other[I]) | (Old[I] & Other[I])
                               : (Other[I] & Old[I]) | (A[I] & ~Old[I]);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegisterBytes(mipsreg::w(0), A);
        E.setRegisterBytes(mipsreg::w(1), B);
        E.setRegisterBytes(mipsreg::w(2), C);
        ASSERT_TRUE(msaExecute(D, GetParam(),
                               0x7880001e | (Operation << 21) |
                                   (Destination << 6) |
                                   (SameSource ? 0 : 1 << 16),
                               E));
        EXPECT_EQ(E.getRegisterBytes(mipsreg::w(Destination)), Expected);
      }
  for (uint32_t Encoding : {0x7880001eu, 0x78a0001eu, 0x78c0001eu}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegisterBytes(mipsreg::w(0), A);
    E.setRegisterBytes(mipsreg::w(1), B);
    EXPECT_FALSE(msaExecute(D, GetParam(), Encoding | (2 << 6) | (1 << 16), E));
  }
}

INSTANTIATE_TEST_SUITE_P(Endian, MIPSMSA,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));
} // namespace

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

#include "llvm/Support/ErrorHandling.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <algorithm>
#include <array>
#include <ios>
#include <memory>
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
  Equal,
  SignedLess,
  UnsignedLess,
  SignedLessEqual,
  UnsignedLessEqual
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
    {{0xffffffff, 0xffffffff, 0, 0}, {1, 0, 1, 0}},
    {{0x00ff00ff, 0x0000ffff, 0x7fffffff, 0x80000000},
     {0x01010101, 0x00010001, 0x87654321, 0x7fffffff}},
    {{0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00},
     {0xf0e0d0c0, 0xb0a09080, 0x70605040, 0x30201001}},
};

std::vector<uint8_t> vectorExpected(const VectorCase &Case,
                                    llvm::ArrayRef<uint8_t> A,
                                    llvm::ArrayRef<uint8_t> B) {
  // Byte-wise carry/borrow is independent of the lifter's sized lane ops.
  std::vector<uint8_t> Result(16);
  if (Case.Operation >= VectorOperation::Equal) {
    const bool Signed = Case.Operation == VectorOperation::SignedLess ||
                        Case.Operation == VectorOperation::SignedLessEqual;
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
      const bool True = Case.Operation == VectorOperation::Equal ? Order == 0
                        : Inclusive                              ? Order <= 0
                                                                 : Order < 0;
      std::fill_n(Result.begin() + Base, Case.ElementBytes, True ? 255 : 0);
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
          const uint32_t Word =
              Case.Encoding | (Destination << 6) | (SameSource ? 0 : (1 << 16));
          ASSERT_TRUE(msaExecute(Decode, GetParam(), Word, Emulator));
          EXPECT_EQ(Emulator.getRegisterBytes(mipsreg::w(Destination)),
                    vectorExpected(Case, A, SameSource ? A : B));
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
  for (uint32_t Word : {0x7b03201eu, 0x79342819u, 0x78f00099u, 0x78000012u}) {
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
          EXPECT_EQ(Actual, vectorExpected(Case, A, SameSource ? A : B));
        }
#endif
}

INSTANTIATE_TEST_SUITE_P(Endian, MIPSMSA,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));
} // namespace

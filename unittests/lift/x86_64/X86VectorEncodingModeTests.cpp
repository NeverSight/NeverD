//===- X86VectorEncodingModeTests.cpp - VEX register-bank semantics ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// In 32-bit mode ignored extension fields must select the low register bank
/// for both register and memory operands, with the same numerical result.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
std::vector<LowOp> lift(const std::vector<uint8_t> &Bytes) {
  Decoder D;
  EXPECT_TRUE(D.init(Arch::X86));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
            Bytes.size());
  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(D.liftToLow(I, Ops));
  return Ops;
}

BinaryImage image(const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Segment Memory;
  Memory.VA = 0x4000;
  Memory.Size = Memory.FileSz = Bytes.size();
  Memory.Data = Bytes;
  Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

TEST(X86VectorEncodingMode, F16CIgnoresBForRegisterAndMemoryOperands) {
  const std::vector<uint8_t> Half = {0, 0x3c, 0, 0xc0, 0, 0, 0, 0x7c};
  const std::array<uint32_t, 4> Expected = {0x3f800000, 0xc0000000, 0,
                                            0x7f800000};
  for (const bool ExtendedB : {false, true})
    for (const bool Memory : {false, true}) {
      SCOPED_TRACE(ExtendedB);
      SCOPED_TRACE(Memory);
      const auto Ops =
          lift({0xc4, static_cast<uint8_t>(ExtendedB ? 0xc2 : 0xe2), 0x79, 0x13,
                static_cast<uint8_t>(Memory ? 0x00 : 0xc1)});
      ASSERT_FALSE(Ops.empty());
      NdOpEmulator Emu(image(Half));
      Emu.setStrictMode(true);
      Emu.setMXCSR(0x1f80);
      Emu.setRegister(x86reg::RAX, 0x4000);
      std::vector<uint8_t> Source = Half;
      Source.resize(16);
      Emu.setRegisterBytes(x86reg::XMM1, Source);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emu.step(Op));
      const auto Result = Emu.getRegisterBytes(x86reg::XMM0);
      ASSERT_TRUE(Result);
      ASSERT_GE(Result->size(), sizeof(Expected));
      EXPECT_EQ(std::memcmp(Result->data(), Expected.data(), sizeof(Expected)),
                0);
      EXPECT_EQ(Emu.getMXCSR(), 0x1f80u);
    }
}

TEST(X86VectorEncodingMode, FMA4IgnoresBAndUsedVvvvHighBit) {
  std::vector<uint8_t> Addend(16);
  const std::array<uint32_t, 4> Four = {0x40800000, 0x40800000, 0x40800000,
                                        0x40800000};
  std::memcpy(Addend.data(), Four.data(), 16);
  for (const bool ExtendedB : {false, true})
    for (const bool ExtendedV : {false, true})
      for (const bool Memory : {false, true}) {
        SCOPED_TRACE(ExtendedB);
        SCOPED_TRACE(ExtendedV);
        SCOPED_TRACE(Memory);
        const auto Ops =
            lift({0xc4, static_cast<uint8_t>(ExtendedB ? 0xc3 : 0xe3),
                  static_cast<uint8_t>(ExtendedV ? 0xb9 : 0xf9), 0x68,
                  static_cast<uint8_t>(Memory ? 0x00 : 0xc2), 0x10});
        ASSERT_FALSE(Ops.empty());
        NdOpEmulator Emu(image(Addend));
        Emu.setStrictMode(true);
        Emu.setMXCSR(0x1f80);
        Emu.setRegister(x86reg::RAX, 0x4000);
        for (unsigned I = 0; I != 3; ++I) {
          const uint32_t Bits = I == 0   ? 0x40000000
                                : I == 1 ? 0x40400000
                                         : 0x40800000;
          const std::array<uint32_t, 4> Values = {Bits, Bits, Bits, Bits};
          Emu.setRegisterBytes(
              x86reg::vectorReg(I),
              {reinterpret_cast<const uint8_t *>(Values.data()), 16});
        }
        for (const auto &Op : Ops)
          ASSERT_TRUE(Emu.step(Op));
        const auto Result = Emu.getRegisterBytes(x86reg::XMM0);
        ASSERT_TRUE(Result);
        ASSERT_GE(Result->size(), 16u);
        std::array<uint32_t, 4> Words;
        std::memcpy(Words.data(), Result->data(), 16);
        for (const auto Word : Words)
          EXPECT_EQ(Word, 0x41200000u); // 2 * 3 + 4
        EXPECT_EQ(Emu.getMXCSR(), 0x1f80u);
      }
}

TEST(X86VectorEncodingMode, EvexHalfConversionIgnoresRegisterExtensions) {
  for (const unsigned Width : {16u, 64u})
    for (const bool Memory : {false, true})
      for (const uint8_t P0 : {0xf3, 0xe3, 0xd3, 0xc3}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Memory);
        SCOPED_TRACE(static_cast<unsigned>(P0));
        const auto Ops =
            lift({0x62, P0, 0x7d,
                  static_cast<uint8_t>(0x0b | (Width == 64 ? 0x40 : 0) |
                                       (Memory ? 0 : 0x80)),
                  0x1d, static_cast<uint8_t>(Memory ? 0x18 : 0xd9), 0x04});
        ASSERT_FALSE(Ops.empty());
        NdOpEmulator Emu(image(std::vector<uint8_t>(Width / 2, 0xa5)));
        Emu.setStrictMode(true);
        Emu.setMXCSR(0x1f80);
        Emu.setRegister(x86reg::RAX, 0x4000);
        Emu.setRegister(x86reg::K3, 0x5555);
        std::array<uint32_t, 16> Source;
        Source.fill(0x3f800000); // exactly 1.0f
        Emu.setRegisterBytes(
            x86reg::XMM3,
            {reinterpret_cast<const uint8_t *>(Source.data()), 64});
        Emu.setRegisterBytes(x86reg::XMM1, std::vector<uint8_t>(64, 0xcc));
        for (const auto &Op : Ops)
          ASSERT_TRUE(Emu.step(Op));
        if (Memory) {
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(x86reg::XMM1, Width / 2);
          Read.addInput(NdVar::cst(0x4000, 8));
          ASSERT_TRUE(Emu.step(Read));
        }
        const auto Result = Emu.getRegisterBytes(x86reg::XMM1);
        ASSERT_TRUE(Result);
        ASSERT_GE(Result->size(), Width / 2);
        for (unsigned Lane = 0; Lane < Width / 4; ++Lane) {
          uint16_t Half;
          std::memcpy(&Half, Result->data() + Lane * 2, 2);
          EXPECT_EQ(Half, Lane % 2 == 0 ? 0x3c00 : (Memory ? 0xa5a5 : 0));
        }
        if (!Memory) {
          ASSERT_EQ(Result->size(), 64u);
          for (unsigned Byte = Width / 2; Byte < 64; ++Byte)
            EXPECT_EQ((*Result)[Byte], 0);
        }
        EXPECT_EQ(Emu.getMXCSR(), 0x1f80u);
      }
}

TEST(X86VectorEncodingMode, EvexIntegerOperandsUseTheLowBankAndAddress) {
  std::array<uint32_t, 16> Left, Right;
  for (unsigned Lane = 0; Lane < 16; ++Lane) {
    Left[Lane] = 7 + Lane;
    Right[Lane] = 11 + Lane;
  }
  std::vector<uint8_t> Data(64);
  std::memcpy(Data.data(), Right.data(), 64);
  for (const uint8_t P0 : {0xf1, 0xe1, 0xd1, 0xc1})
    for (const bool ExtendedV : {false, true})
      for (const bool Memory : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(P0));
        SCOPED_TRACE(ExtendedV);
        SCOPED_TRACE(Memory);
        const auto Ops =
            lift({0x62, P0, static_cast<uint8_t>(ExtendedV ? 0x35 : 0x75), 0x4b,
                  0xfe, static_cast<uint8_t>(Memory ? 0x00 : 0xc2)});
        ASSERT_FALSE(Ops.empty());
        NdOpEmulator Emu(image(Data));
        Emu.setStrictMode(true);
        Emu.setRegister(x86reg::RAX, 0x4000);
        Emu.setRegister(x86reg::K3, 0xa55a);
        Emu.setRegisterBytes(x86reg::XMM0, std::vector<uint8_t>(64, 0xcc));
        Emu.setRegisterBytes(
            x86reg::XMM1, {reinterpret_cast<const uint8_t *>(Left.data()), 64});
        Emu.setRegisterBytes(x86reg::XMM2, Data);
        for (const auto &Op : Ops)
          ASSERT_TRUE(Emu.step(Op));
        const auto Result = Emu.getRegisterBytes(x86reg::XMM0);
        ASSERT_TRUE(Result);
        ASSERT_EQ(Result->size(), 64u);
        std::array<uint32_t, 16> Actual;
        std::memcpy(Actual.data(), Result->data(), 64);
        for (unsigned Lane = 0; Lane < 16; ++Lane)
          EXPECT_EQ(Actual[Lane], (0xa55a & (1u << Lane))
                                      ? Left[Lane] + Right[Lane]
                                      : 0xcccccccc);
      }
}

TEST(X86VectorEncodingMode, ReservedEvexFieldsAndHighBankDetailRemainRejected) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X86));
  const uint8_t Bytes[] = {0x62, 0xf3, 0x7d, 0x08, 0x1d, 0xd9, 0x04};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, I), sizeof(Bytes));
  ASSERT_NE(I.Raw, nullptr);
  ASSERT_NE(I.Raw->detail, nullptr);
  for (unsigned Case = 0; Case < 7; ++Case) {
    SCOPED_TRACE(Case);
    cs_insn Raw = *I.Raw;
    cs_detail Detail = *Raw.detail;
    Raw.detail = &Detail;
    switch (Case) {
    case 0:
      Raw.bytes[1] ^= 0x80; // BOUND disambiguation, not an ignored R bit
      break;
    case 1:
      Raw.bytes[1] ^= 0x40;
      break;
    case 2:
      Raw.bytes[3] ^= 0x08; // V' must encode one outside long mode
      break;
    case 3:
      Raw.bytes[1] |= 0x08; // APX address extensions are long-mode only
      break;
    case 4:
      Raw.bytes[2] &= ~0x04;
      break;
    case 5:
      Raw.bytes[2] ^= 0x08; // unused vvvv remains reserved
      break;
    case 6:
      Raw.bytes[1] &= ~0x10;
      Detail.x86.operands[1].reg = X86_REG_XMM19;
      break;
    }
    std::copy_n(Raw.bytes, 4, Detail.x86.opcode);
    X86Lifter L(Arch::X86);
    L.setStrict(true);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}
} // namespace

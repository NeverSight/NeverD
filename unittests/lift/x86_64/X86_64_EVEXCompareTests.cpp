//===- X86_64_EVEXCompareTests.cpp - EVEX compare semantics ------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  const int Size =
      Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn);
  if (Size != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "x86-64 instruction decode failed";
    return {};
  }

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

void expectStrictlyLifted(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));

  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
            static_cast<int>(Bytes.size()));

  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(Dec.liftToLow(Insn, Ops));
  EXPECT_FALSE(Ops.empty());
}

template <size_t N>
std::vector<uint8_t> dwordVector(const std::array<int32_t, N> &Lanes) {
  std::vector<uint8_t> Bytes(N * sizeof(int32_t));
  for (size_t Lane = 0; Lane < N; ++Lane) {
    const uint32_t Value = static_cast<uint32_t>(Lanes[Lane]);
    for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
      Bytes[Lane * sizeof(Value) + Byte] =
          static_cast<uint8_t>(Value >> (Byte * 8));
  }
  return Bytes;
}

void expectKResult(const std::vector<uint8_t> &Encoding,
                   const std::vector<uint8_t> &Left,
                   const std::vector<uint8_t> &Right, uint64_t Expected,
                   std::optional<uint64_t> WriteMask = std::nullopt) {
  ASSERT_EQ(Left.size(), Right.size());
  ASSERT_TRUE(Left.size() == 16 || Left.size() == 32 || Left.size() == 64);

  std::vector<LowOp> Ops;
  try {
    Ops = liftX64(Encoding);
  } catch (const std::exception &Error) {
    FAIL() << Error.what();
  }
  ASSERT_FALSE(Ops.empty());

  x86_reg LeftReg = X86_REG_ZMM2;
  x86_reg RightReg = X86_REG_ZMM3;
  if (Left.size() == 16) {
    LeftReg = X86_REG_XMM2;
    RightReg = X86_REG_XMM3;
  } else if (Left.size() == 32) {
    LeftReg = X86_REG_YMM2;
    RightReg = X86_REG_YMM3;
  }

  const RegInfo Source1 = mapCapstoneReg(LeftReg);
  const RegInfo Source2 = mapCapstoneReg(RightReg);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source1.Offset, Left);
  Emulator.setRegisterBytes(Source2.Offset, Right);
  if (WriteMask)
    Emulator.setRegister(Mask.Offset, *WriteMask);
  Emulator.setRegister(Destination.Offset, UINT64_MAX);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Expected);
}

void setLane(std::vector<uint8_t> &Value, size_t Lane, size_t ElementSize,
             uint64_t Element) {
  const size_t Offset = Lane * ElementSize;
  ASSERT_LE(Offset + ElementSize, Value.size());
  for (size_t Byte = 0; Byte < ElementSize; ++Byte)
    Value[Offset + Byte] = static_cast<uint8_t>(Element >> (Byte * 8));
}

TEST(X86EVEXCompare, VpcmpdEqBuildsOneKBitPerSignedDwordLane) {
  // vpcmpeqd k1, zmm2, zmm3
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf3, 0x6d, 0x48, 0x1f, 0xcb, 0x00});
  ASSERT_FALSE(Ops.empty());

  const std::array<int32_t, 16> Left = {
      0, -1, 2, INT32_MIN, 4, -5, 6, INT32_MAX, 8, -9, 10, -11, 12, 13, -14, 15,
  };
  const std::array<int32_t, 16> Right = {
      0, 1, 2, INT32_MIN, -4, -5, 7, INT32_MAX, -8, -9, 11, -11, 0, 13, 14, 15,
  };

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source1.Offset, dwordVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, dwordVector(Right));
  Emulator.setRegister(Destination.Offset, UINT64_MAX);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), UINT64_C(0xAAAD));
}

TEST(X86EVEXCompare, VpcmpdLtUsesSignedDwordOrdering) {
  // vpcmpltd k1, zmm2, zmm3
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf3, 0x6d, 0x48, 0x1f, 0xcb, 0x01});
  ASSERT_FALSE(Ops.empty());

  const std::array<int32_t, 16> Left = {
      -1, 1, INT32_MIN, INT32_MAX, 0, -5, 5, -100, 100, -2, 2, -3, 3, -4, 4, 7,
  };
  const std::array<int32_t, 16> Right = {
      0, 0, INT32_MAX, INT32_MIN, 0, -4, 4, -100, 99, -1, 3, -4, 3, 4, -4, 8,
  };

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source1.Offset, dwordVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, dwordVector(Right));
  Emulator.setRegister(Destination.Offset, UINT64_MAX);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), UINT64_C(0xA625));
}

TEST(X86EVEXCompare, VpcmpdNleZeroMasksKResultAndClearsHighBits) {
  // vpcmpnled k1 {k2}, zmm2, zmm3
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf3, 0x6d, 0x4a, 0x1f, 0xcb, 0x06});
  ASSERT_FALSE(Ops.empty());

  const std::array<int32_t, 16> Left = {
      10,  -1,  INT32_MIN, INT32_MAX, 0,  -5, -4,  8,
      -10, -20, 2,         1,         12, 13, -14, 15,
  };
  const std::array<int32_t, 16> Right = {
      0,   -2,  INT32_MAX, INT32_MIN, 0,  -4, -5,  7,
      -20, -10, 1,         2,         11, 14, -15, 14,
  };

  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source1.Offset, dwordVector(Left));
  Emulator.setRegisterBytes(Source2.Offset, dwordVector(Right));
  Emulator.setRegister(WriteMask.Offset, UINT64_C(0xFFFF5AA5));
  Emulator.setRegister(Destination.Offset, UINT64_MAX);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
  EXPECT_EQ(*Emulator.getRegister(Destination.Offset), UINT64_C(0x5081));
}

TEST(X86EVEXCompare, VpcmpdImplementsEveryIntegerPredicateCode) {
  const std::array<int32_t, 4> Left = {-2, -1, 1, 2};
  const std::array<int32_t, 4> Right = {-2, 0, 0, 2};
  const std::array<uint64_t, 8> Expected = {
      UINT64_C(0x9), UINT64_C(0x2), UINT64_C(0xB), UINT64_C(0x0),
      UINT64_C(0x6), UINT64_C(0xD), UINT64_C(0x4), UINT64_C(0xF),
  };

  const RegInfo Source1 = mapCapstoneReg(X86_REG_XMM2);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_XMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);

  for (uint8_t Predicate = 0; Predicate < Expected.size(); ++Predicate) {
    SCOPED_TRACE(testing::Message()
                 << "predicate=" << static_cast<unsigned>(Predicate));
    // vpcmpd k1, xmm2, xmm3, imm8
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf3, 0x6d, 0x08, 0x1f, 0xcb, Predicate});
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Source1.Offset, dwordVector(Left));
    Emulator.setRegisterBytes(Source2.Offset, dwordVector(Right));
    Emulator.setRegister(Destination.Offset, UINT64_MAX);

    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
    EXPECT_EQ(*Emulator.getRegister(Destination.Offset), Expected[Predicate]);
  }
}

TEST(X86EVEXCompare, VpcmpMemoryFormsLiftAfterCanonicalValidation) {
  // vpcmpeqb k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0x6d, 0x48, 0x3f, 0x08, 0x00});
  // vpcmpequb k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0x6d, 0x48, 0x3e, 0x08, 0x00});
  // vpcmpeqw k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0xed, 0x48, 0x3f, 0x08, 0x00});
  // vpcmpequw k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0xed, 0x48, 0x3e, 0x08, 0x00});
  // vpcmpeqd k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0x6d, 0x48, 0x1f, 0x08, 0x00});
  // vpcmpequd k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0x6d, 0x48, 0x1e, 0x08, 0x00});
  // vpcmpeqq k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0xed, 0x48, 0x1f, 0x08, 0x00});
  // vpcmpequq k1, zmm2, zmmword ptr [rax]
  expectStrictlyLifted({0x62, 0xf3, 0xed, 0x48, 0x1e, 0x08, 0x00});
  // vpcmpeqd k1, zmm2, dword ptr [rax]{1to16}
  expectStrictlyLifted({0x62, 0xf3, 0x6d, 0x58, 0x1f, 0x08, 0x00});
}

TEST(X86EVEXCompare, VpcmpdRejectsReservedEvexZeroingBit) {
  // Mask-producing compares support a zeroing writemask only; EVEX.z is
  // reserved and must not decode as a separate merge/zero choice.
  const std::vector<uint8_t> Bytes = {0x62, 0xf3, 0x6d, 0xca, 0x1f, 0xcb, 0x06};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
            0);
}

TEST(X86EVEXCompare, VpcmpbLtUsesSignedByteOrderingAcrossAllZmmLanes) {
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = (Lane & 1) == 0 ? UINT8_C(0x80) : UINT8_C(0x00);
    Right[Lane] = (Lane & 1) == 0 ? UINT8_C(0x00) : UINT8_C(0x80);
  }

  // vpcmpltb k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0x6d, 0x48, 0x3f, 0xcb, 0x01}, Left, Right,
                UINT64_C(0x5555555555555555));
}

TEST(X86EVEXCompare, VpcmpubLtUsesUnsignedByteOrderingAcrossAllZmmLanes) {
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  for (size_t Lane = 0; Lane < Left.size(); ++Lane) {
    Left[Lane] = (Lane & 1) == 0 ? UINT8_C(0x80) : UINT8_C(0x00);
    Right[Lane] = (Lane & 1) == 0 ? UINT8_C(0x00) : UINT8_C(0x80);
  }

  // vpcmpltub k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0x6d, 0x48, 0x3e, 0xcb, 0x01}, Left, Right,
                UINT64_C(0xAAAAAAAAAAAAAAAA));
}

TEST(X86EVEXCompare, VpcmpwAndVpcmpuwDisagreeAtTheWordSignBoundary) {
  constexpr size_t ElementSize = 2;
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  for (size_t Lane = 0; Lane < Left.size() / ElementSize; ++Lane) {
    setLane(Left, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0x8000) : UINT64_C(0));
    setLane(Right, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0) : UINT64_C(0x8000));
  }

  // vpcmpltw k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0xed, 0x48, 0x3f, 0xcb, 0x01}, Left, Right,
                UINT64_C(0x55555555));
  // vpcmpltuw k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0xed, 0x48, 0x3e, 0xcb, 0x01}, Left, Right,
                UINT64_C(0xAAAAAAAA));
}

TEST(X86EVEXCompare, VpcmpdAndVpcmpudDisagreeAtTheDwordSignBoundary) {
  constexpr size_t ElementSize = 4;
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  for (size_t Lane = 0; Lane < Left.size() / ElementSize; ++Lane) {
    setLane(Left, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0x80000000) : UINT64_C(0));
    setLane(Right, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0) : UINT64_C(0x80000000));
  }

  // vpcmpltd k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0x6d, 0x48, 0x1f, 0xcb, 0x01}, Left, Right,
                UINT64_C(0x5555));
  // vpcmpltud k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0x6d, 0x48, 0x1e, 0xcb, 0x01}, Left, Right,
                UINT64_C(0xAAAA));
}

TEST(X86EVEXCompare, VpcmpqAndVpcmpuqUseExact64BitSignedness) {
  constexpr size_t ElementSize = 8;
  std::vector<uint8_t> Left(64);
  std::vector<uint8_t> Right(64);
  for (size_t Lane = 0; Lane < Left.size() / ElementSize; ++Lane) {
    setLane(Left, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0x8000000000000000) : UINT64_C(0));
    setLane(Right, Lane, ElementSize,
            (Lane & 1) == 0 ? UINT64_C(0) : UINT64_C(0x8000000000000000));
  }

  // vpcmpltq k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0xed, 0x48, 0x1f, 0xcb, 0x01}, Left, Right,
                UINT64_C(0x55));
  // vpcmpltuq k1, zmm2, zmm3
  expectKResult({0x62, 0xf3, 0xed, 0x48, 0x1e, 0xcb, 0x01}, Left, Right,
                UINT64_C(0xAA));
}

TEST(X86EVEXCompare, EveryIntegerFamilySupportsAllPredicatesAndVectorWidths) {
  struct Family {
    const char *Name;
    uint8_t EvexP1;
    uint8_t Opcode;
    size_t ElementSize;
  };
  const std::array<Family, 8> Families = {{
      {"vpcmpb", 0x6d, 0x3f, 1},
      {"vpcmpub", 0x6d, 0x3e, 1},
      {"vpcmpw", 0xed, 0x3f, 2},
      {"vpcmpuw", 0xed, 0x3e, 2},
      {"vpcmpd", 0x6d, 0x1f, 4},
      {"vpcmpud", 0x6d, 0x1e, 4},
      {"vpcmpq", 0xed, 0x1f, 8},
      {"vpcmpuq", 0xed, 0x1e, 8},
  }};
  struct Width {
    const char *Name;
    uint8_t EvexP2;
    size_t Bytes;
  };
  const std::array<Width, 3> Widths = {{
      {"xmm", 0x08, 16},
      {"ymm", 0x28, 32},
      {"zmm", 0x48, 64},
  }};

  for (const Family &F : Families) {
    for (const Width &W : Widths) {
      std::vector<uint8_t> Left(W.Bytes);
      std::vector<uint8_t> Right(W.Bytes);
      const size_t Lanes = W.Bytes / F.ElementSize;
      for (size_t Lane = 0; Lane < Lanes; ++Lane) {
        setLane(Left, Lane, F.ElementSize, 1);
        setLane(Right, Lane, F.ElementSize, 2);
      }
      const uint64_t AllLanes =
          Lanes == 64 ? UINT64_MAX : (UINT64_C(1) << Lanes) - 1;

      for (uint8_t Predicate = 0; Predicate < 8; ++Predicate) {
        SCOPED_TRACE(testing::Message()
                     << F.Name << '/' << W.Name
                     << "/predicate=" << static_cast<unsigned>(Predicate));
        const bool ExpectedTrue = Predicate == 1 || Predicate == 2 ||
                                  Predicate == 4 || Predicate == 7;
        expectKResult(
            {0x62, 0xf3, F.EvexP1, W.EvexP2, F.Opcode, 0xcb, Predicate}, Left,
            Right, ExpectedTrue ? AllLanes : 0);
      }
    }
  }
}

TEST(X86EVEXCompare, EveryIntegerFamilyUsesAZeroingKWriteMask) {
  struct Family {
    uint8_t EvexP1;
    uint8_t Opcode;
    size_t ElementSize;
  };
  const std::array<Family, 8> Families = {{
      {0x6d, 0x3f, 1},
      {0x6d, 0x3e, 1},
      {0xed, 0x3f, 2},
      {0xed, 0x3e, 2},
      {0x6d, 0x1f, 4},
      {0x6d, 0x1e, 4},
      {0xed, 0x1f, 8},
      {0xed, 0x1e, 8},
  }};
  struct Width {
    uint8_t EvexP2;
    size_t Bytes;
  };
  const std::array<Width, 3> Widths = {{
      {0x08, 16},
      {0x28, 32},
      {0x48, 64},
  }};
  constexpr uint64_t WriteMask = UINT64_C(0xA55AA55AA55AA55A);

  for (const Family &F : Families) {
    for (const Width &W : Widths) {
      std::vector<uint8_t> Left(W.Bytes);
      std::vector<uint8_t> Right(W.Bytes);
      const size_t Lanes = W.Bytes / F.ElementSize;
      for (size_t Lane = 0; Lane < Lanes; ++Lane) {
        setLane(Left, Lane, F.ElementSize, 1);
        setLane(Right, Lane, F.ElementSize, 2);
      }
      const uint64_t ActiveLanes =
          Lanes == 64 ? UINT64_MAX : (UINT64_C(1) << Lanes) - 1;

      // LT is true in every lane; aaa=2 supplies K2. Every inactive K2 bit
      // must clear the corresponding destination bit rather than merge K1.
      expectKResult({0x62, 0xf3, F.EvexP1, static_cast<uint8_t>(W.EvexP2 | 2),
                     F.Opcode, 0xcb, 0x01},
                    Left, Right, WriteMask & ActiveLanes, WriteMask);
    }
  }
}

} // namespace

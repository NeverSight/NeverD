//===- X86_64_EVEXMaskConversionTests.cpp - EVEX mask conversions -------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

std::vector<uint8_t> maskConversionEncoding(bool MaskToVector,
                                            size_t ElementSize,
                                            size_t VectorSize) {
  const uint8_t WidthByte = ElementSize == 2 || ElementSize == 8 ? 0xfe : 0x7e;
  const uint8_t LengthByte =
      VectorSize == 16 ? 0x08 : (VectorSize == 32 ? 0x28 : 0x48);
  const bool SmallElement = ElementSize == 1 || ElementSize == 2;
  const uint8_t Opcode = static_cast<uint8_t>((SmallElement ? 0x28 : 0x38) +
                                              (MaskToVector ? 0 : 1));
  // vpmovm2* vec2, k1 / vpmov*2m k1, vec2.
  const uint8_t ModRM = MaskToVector ? 0xd1 : 0xca;
  return {0x62, 0xf2, WidthByte, LengthByte, Opcode, ModRM};
}

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

void expectDecodeOrLiftRejected(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));

  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size()))
    return;

  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

uint64_t lanePattern(size_t Lane) {
  return (UINT64_C(0xd6a5c39b4e71820f) >> (Lane % 16)) & 1;
}

TEST(X86EVEXMaskConversion,
     MaskBitsExpandAcrossEveryElementWidthAndVectorLength) {
  constexpr std::array<size_t, 4> ElementSizes = {1, 2, 4, 8};
  constexpr std::array<size_t, 3> VectorSizes = {16, 32, 64};

  const RegInfo Mask = mapCapstoneReg(X86_REG_K1);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM2);
  ASSERT_EQ(Mask.Size, 8u);
  ASSERT_EQ(Destination.Size, 64u);

  for (size_t ElementSize : ElementSizes) {
    for (size_t VectorSize : VectorSizes) {
      SCOPED_TRACE(testing::Message() << "element bytes=" << ElementSize
                                      << ", vector bytes=" << VectorSize);
      const std::vector<LowOp> Ops =
          liftX64(maskConversionEncoding(true, ElementSize, VectorSize));
      ASSERT_FALSE(Ops.empty());

      const size_t LaneCount = VectorSize / ElementSize;
      uint64_t MaskValue = 0;
      for (size_t Lane = 0; Lane < LaneCount; ++Lane)
        MaskValue |= lanePattern(Lane) << Lane;

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setRegister(Mask.Offset, MaskValue);
      Emulator.setRegisterBytes(Destination.Offset,
                                std::vector<uint8_t>(64, 0x5a));
      EXPECT_EQ(Emulator.run(Ops), Ops.size());

      const auto Result = Emulator.getRegisterBytes(Destination.Offset);
      ASSERT_TRUE(Result);
      ASSERT_EQ(Result->size(), 64u);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        const uint8_t Expected = lanePattern(Lane) ? 0xff : 0x00;
        for (size_t Byte = 0; Byte < ElementSize; ++Byte)
          EXPECT_EQ((*Result)[Lane * ElementSize + Byte], Expected);
      }
      for (size_t Byte = VectorSize; Byte < 64; ++Byte)
        EXPECT_EQ((*Result)[Byte], 0u);
    }
  }
}

TEST(X86EVEXMaskConversion,
     LaneSignBitsCompressAndClearUnusedOpmaskBitsAtEveryWidth) {
  constexpr std::array<size_t, 4> ElementSizes = {1, 2, 4, 8};
  constexpr std::array<size_t, 3> VectorSizes = {16, 32, 64};

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM2);
  const RegInfo Destination = mapCapstoneReg(X86_REG_K1);
  ASSERT_EQ(Source.Size, 64u);
  ASSERT_EQ(Destination.Size, 8u);

  for (size_t ElementSize : ElementSizes) {
    for (size_t VectorSize : VectorSizes) {
      SCOPED_TRACE(testing::Message() << "element bytes=" << ElementSize
                                      << ", vector bytes=" << VectorSize);
      const std::vector<LowOp> Ops =
          liftX64(maskConversionEncoding(false, ElementSize, VectorSize));
      ASSERT_FALSE(Ops.empty());

      std::vector<uint8_t> SourceValue(64, 0xff);
      const size_t LaneCount = VectorSize / ElementSize;
      uint64_t ExpectedMask = 0;
      for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
        for (size_t Byte = 0; Byte < ElementSize; ++Byte)
          SourceValue[Lane * ElementSize + Byte] =
              static_cast<uint8_t>(0x21 + Lane + Byte);
        if (lanePattern(Lane)) {
          SourceValue[(Lane + 1) * ElementSize - 1] |= 0x80;
          ExpectedMask |= UINT64_C(1) << Lane;
        } else {
          SourceValue[(Lane + 1) * ElementSize - 1] &= 0x7f;
        }
      }

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setRegisterBytes(Source.Offset, SourceValue);
      Emulator.setRegister(Destination.Offset, UINT64_MAX);
      EXPECT_EQ(Emulator.run(Ops), Ops.size());
      ASSERT_TRUE(Emulator.getRegister(Destination.Offset));
      EXPECT_EQ(*Emulator.getRegister(Destination.Offset), ExpectedMask);
      const auto SourceAfter = Emulator.getRegisterBytes(Source.Offset);
      ASSERT_TRUE(SourceAfter);
      EXPECT_EQ(*SourceAfter, SourceValue);
    }
  }
}

TEST(X86EVEXMaskConversion, HighestVectorAndOpmaskRegistersRemainAddressable) {
  const RegInfo Vector = mapCapstoneReg(X86_REG_ZMM31);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K7);
  ASSERT_EQ(Vector.Size, 64u);
  ASSERT_EQ(Mask.Size, 8u);

  {
    // vpmovm2b zmm31, k7
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0x62, 0x7e, 0x48, 0x28, 0xff});
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegister(Mask.Offset, UINT64_C(0x8000000000000001));
    Emulator.setRegisterBytes(Vector.Offset, std::vector<uint8_t>(64, 0x5a));
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    const auto Result = Emulator.getRegisterBytes(Vector.Offset);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), 64u);
    for (size_t Lane = 0; Lane < 64; ++Lane)
      EXPECT_EQ((*Result)[Lane], Lane == 0 || Lane == 63 ? 0xff : 0x00);
  }

  {
    // vpmovq2m k7, zmm31
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0x92, 0xfe, 0x48, 0x39, 0xff});
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> SourceValue(64, 0x35);
    SourceValue[15] = 0x80;
    SourceValue[55] = 0x80;
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setRegisterBytes(Vector.Offset, SourceValue);
    Emulator.setRegister(Mask.Offset, UINT64_MAX);
    EXPECT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(Mask.Offset));
    EXPECT_EQ(*Emulator.getRegister(Mask.Offset), UINT64_C(0x42));
    const auto SourceAfter = Emulator.getRegisterBytes(Vector.Offset);
    ASSERT_TRUE(SourceAfter);
    EXPECT_EQ(*SourceAfter, SourceValue);
  }
}

TEST(X86EVEXMaskConversion,
     MemoryAndReservedModifierEncodingsAreRejectedFailClosed) {
  const std::array<std::vector<uint8_t>, 12> Invalid = {{
      // Register-only instructions with memory-shaped ModRM bytes.
      {0x62, 0xf2, 0x7e, 0x08, 0x28, 0x11},
      {0x62, 0xf2, 0x7e, 0x08, 0x29, 0x0a},
      // Nonzero opmask decorator (aaa), zeroing (z), broadcast/RC (b),
      // reserved vvvv and reserved vector length, in both directions.
      {0x62, 0xf2, 0x7e, 0x09, 0x28, 0xd1},
      {0x62, 0xf2, 0x7e, 0x09, 0x29, 0xca},
      {0x62, 0xf2, 0x7e, 0x88, 0x28, 0xd1},
      {0x62, 0xf2, 0x7e, 0x88, 0x29, 0xca},
      {0x62, 0xf2, 0x7e, 0x18, 0x28, 0xd1},
      {0x62, 0xf2, 0x7e, 0x18, 0x29, 0xca},
      {0x62, 0xf2, 0x76, 0x08, 0x28, 0xd1},
      {0x62, 0xf2, 0x76, 0x08, 0x29, 0xca},
      {0x62, 0xf2, 0x7e, 0x68, 0x28, 0xd1},
      {0x62, 0xf2, 0x7e, 0x68, 0x29, 0xca},
  }};

  for (const std::vector<uint8_t> &Bytes : Invalid) {
    SCOPED_TRACE(testing::Message()
                 << "opcode=" << std::hex << static_cast<unsigned>(Bytes[4])
                 << ", evex-p2=" << static_cast<unsigned>(Bytes[3]));
    expectDecodeOrLiftRejected(Bytes);
  }
}

} // namespace

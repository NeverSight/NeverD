//===- X86_64_MPSADBWStrictTests.cpp - MPSADBW strict semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
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

std::vector<uint8_t> wordBytes(const std::array<uint16_t, 16> &Words) {
  std::vector<uint8_t> Bytes(Words.size() * sizeof(uint16_t));
  for (size_t I = 0; I < Words.size(); ++I) {
    Bytes[I * 2] = static_cast<uint8_t>(Words[I]);
    Bytes[I * 2 + 1] = static_cast<uint8_t>(Words[I] >> 8);
  }
  return Bytes;
}

TEST(X86MPSADBWStrict, YmmUsesIndependentControlForEach128BitLane) {
  // vmpsadbw ymm1, ymm2, ymm3, 0x15
  // Low lane control 5 selects src1 byte offset 4 and src2 block 1.
  // High lane control 2 selects src1 byte offset 0 and src2 block 2.
  const std::vector<LowOp> Ops = liftX64({0xc4, 0xe3, 0x6d, 0x42, 0xcb, 0x15});
  ASSERT_FALSE(Ops.empty());

  const std::vector<uint8_t> Source1 = {
      10,  20,  30,  40,  50,  60,  70,  80,  90,  100, 110,
      120, 130, 140, 150, 160, 200, 190, 180, 170, 160, 150,
      140, 130, 120, 110, 100, 90,  80,  70,  60,  50,
  };
  const std::vector<uint8_t> Source2 = {
      1, 2, 3, 4, 51, 62, 73, 84, 9,   10,  11,  12,  13, 14, 15, 16,
      1, 2, 3, 4, 5,  6,  7,  8,  195, 185, 175, 165, 9,  10, 11, 12,
  };
  const std::vector<uint8_t> Expected = wordBytes({
      10,
      30,
      70,
      110,
      150,
      190,
      230,
      270,
      20,
      20,
      60,
      100,
      140,
      180,
      220,
      260,
  });

  const RegInfo Left = mapCapstoneReg(X86_REG_YMM2);
  const RegInfo Right = mapCapstoneReg(X86_REG_YMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_YMM1);
  ASSERT_EQ(Left.Size, 32u);
  ASSERT_EQ(Right.Size, 32u);
  ASSERT_EQ(Destination.Size, 32u);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(Left.Offset, Source1);
  Emulator.setRegisterBytes(Right.Offset, Source2);

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_FALSE(Emulator.skips().any());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  ASSERT_GE(Result->size(), Expected.size());
  EXPECT_TRUE(std::equal(Expected.begin(), Expected.end(), Result->begin()));
}

} // namespace

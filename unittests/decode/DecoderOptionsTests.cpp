//===- DecoderOptionsTests.cpp - Decoder option and lifetime parity ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify scan-to-detail transitions, fallback, failed init and mode changes.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"

#include <cstring>
#include <vector>

namespace neverd {
namespace {

void compare(Decoder &D, Arch A, const std::vector<uint8_t> &Bytes, bool Detail,
             bool Text, bool Light) {
  const cs_arch CA = A == Arch::ARM       ? CS_ARCH_ARM
                     : A == Arch::AArch64 ? CS_ARCH_AARCH64
                                          : CS_ARCH_X86;
  const cs_mode CM = A == Arch::X64   ? CS_MODE_64
                     : A == Arch::X86 ? CS_MODE_32
                     : A == Arch::ARM
                         ? static_cast<cs_mode>(CS_MODE_ARM | CS_MODE_V8)
                         : CS_MODE_ARM;
  csh Handle = 0;
  ASSERT_EQ(cs_open(CA, CM, &Handle), CS_ERR_OK);
  cs_option(Handle, CS_OPT_DETAIL, Detail ? CS_OPT_ON : CS_OPT_OFF);
  cs_option(Handle, CS_OPT_TEXT, Text ? CS_OPT_ON : CS_OPT_OFF);
  cs_insn *Expected = cs_malloc(Handle);
  ASSERT_NE(Expected, nullptr);
  D.setDetail(Detail);
  D.setText(Text);
  const uint8_t *P = Bytes.data();
  size_t Remaining = Bytes.size();
  uint64_t Address = 0x100000;
  const bool Valid = cs_disasm_iter(Handle, &P, &Remaining, &Address, Expected);
  DecodedInsn Actual{};
  const int Size =
      Light ? D.decodeOneLight(Bytes.data(), Bytes.size(), 0x100000, Actual)
            : D.decodeOne(Bytes.data(), Bytes.size(), 0x100000, Actual);
  EXPECT_EQ(Size != 0, Valid);
  if (Valid && Size) {
    EXPECT_EQ(Actual.Size, Expected->size);
    EXPECT_EQ(Actual.Id, Expected->id);
    EXPECT_EQ(Actual.Addr, Expected->address);
    EXPECT_EQ(std::memcmp(Actual.Raw->bytes, Expected->bytes, Size), 0);
    EXPECT_STREQ(Actual.Raw->mnemonic, Expected->mnemonic);
    EXPECT_STREQ(Actual.Raw->op_str, Expected->op_str);
    if (Detail && Text) {
      ASSERT_NE(Actual.Raw->detail, nullptr);
      if (CA == CS_ARCH_X86)
        EXPECT_EQ(std::memcmp(&Actual.Raw->detail->x86, &Expected->detail->x86,
                              sizeof(cs_x86)),
                  0);
    }
  }
  cs_free(Expected, 1);
  cs_close(&Handle);
}

TEST(DecoderOptions, ScanDetailTransitionsAndFallback) {
  Decoder D;
  for (Arch A : {Arch::X64, Arch::ARM, Arch::X86, Arch::AArch64, Arch::X64}) {
    ASSERT_TRUE(D.init(A));
    std::vector<std::vector<uint8_t>> Cases;
    if (A == Arch::ARM)
      Cases = {{0x00, 0xf0, 0x20, 0xe3}};
    else if (A == Arch::AArch64)
      Cases = {{0x1f, 0x20, 0x03, 0xd5}};
    else
      Cases = {{0xc5, 0xf8, 0x28, 0x00},
               {0xc4, 0xe2, 0x69, 0xa8, 0x00},
               {0x62, 0xf1, 0x7c, 0x48, 0x28, 0x00},
               {0xc5, 0xf8, 0x58, 0xc1}, // Unmapped VADDPS uses Capstone.
               {0x67, 0xc5, 0xf8, 0x28, 0x00},
               {0x66, 0x48, 0xd9, 0x30},
               {0x66, 0x0f, 0xb9, 0xc1},
               {0xd5, 0x58, 0x31, 0x02}, // APX capability remains in Capstone.
               {0xf0, 0xc5, 0xf8, 0x28, 0x00},
               {0x62},
               {0x90}};
    for (const auto &Bytes : Cases)
      for (bool Light : {false, true})
        for (bool Detail : {false, true, false})
          for (bool Text : {false, true, false})
            compare(D, A, Bytes, Detail, Text, Light);
    // A failed reconfiguration must preserve the currently usable decoder.
    EXPECT_FALSE(D.init(Arch::Unknown));
    compare(D, A, Cases.front(), false, false, true);
    EXPECT_FALSE(D.init(A, InstructionMode::MixedARMThumb));
    compare(D, A, Cases.front(), true, true, false);
  }
  D.reset();
  DecodedInsn Out{};
  const uint8_t Nop = 0x90;
  EXPECT_EQ(D.decodeOneLight(&Nop, 1, 0, Out), 0);
  ASSERT_TRUE(D.init(Arch::X86));
  compare(D, Arch::X86, {Nop}, false, false, true);
}

} // namespace
} // namespace neverd

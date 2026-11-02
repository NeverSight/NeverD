//===- X86SHA512Tests.cpp - SHA512 independent scalar oracle tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare every SHA512 form against scalar SHA512 equations without requiring
/// host SHA512 instructions. Exercise aliases, high registers and upper zeros.
//===----------------------------------------------------------------------===//

#include "X86SHA512Reference.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
using namespace neverd::test;
TEST(X86SHA512, MatchesScalarOracleWithAliasingAndUpperLaneClearing) {
  std::mt19937_64 Random(0x512);
  for (const auto Arch : {Arch::X86, Arch::X64})
    for (const unsigned Opcode : {0xcc, 0xcd, 0xcb})
      for (const unsigned D : {0u, 7u, 8u, 15u}) {
        if (Arch == Arch::X86 && D >= 8)
          continue;
        for (const unsigned S : {D, (D + 1) % (Arch == Arch::X64 ? 16 : 8)})
          for (const unsigned W : {D, S, 2u}) {
            SCOPED_TRACE(Opcode);
            SCOPED_TRACE(D);
            SCOPED_TRACE(S);
            SCOPED_TRACE(W);
            const unsigned Rm = Opcode == 0xcb ? W : S;
            const std::vector<uint8_t> Bytes = {
                0xc4,
                static_cast<uint8_t>(0xe2 ^ ((D & 8) << 4) ^ ((Rm & 8) << 2)),
                static_cast<uint8_t>(0x7f ^ (Opcode == 0xcb ? S << 3 : 0)),
                static_cast<uint8_t>(Opcode),
                static_cast<uint8_t>(0xc0 | ((D & 7) << 3) | (Rm & 7))};
            Decoder Dec;
            ASSERT_TRUE(Dec.init(Arch));
            DecodedInsn Insn{};
            ASSERT_EQ(
                Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                5);
            std::vector<LowOp> Ops;
            ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
            ASSERT_FALSE(Ops.empty());
            for (unsigned Iteration = 0; Iteration != 12; ++Iteration) {
              std::array<Words, 16> Registers;
              BinaryImage Image;
              Image.Arch = Arch;
              NdOpEmulator Emu(Image);
              for (unsigned I = 0; I != Registers.size(); ++I) {
                for (auto &Value : Registers[I])
                  Value = Random();
                Emu.setRegisterBytes(
                    x86reg::vectorReg(I),
                    {reinterpret_cast<const uint8_t *>(Registers[I].data()),
                     64});
              }
              const auto Expected = sha512Reference(Opcode, Registers[D],
                                                    Registers[S], Registers[W]);
              for (const auto F : {x86reg::CF, x86reg::ZF, x86reg::OF,
                                   x86reg::PF, x86reg::SF, x86reg::AF})
                Emu.setRegister(F, 1);
              for (const auto &Op : Ops)
                ASSERT_TRUE(Emu.step(Op)) << static_cast<unsigned>(Op.Opcode);
              const auto Result = Emu.getRegisterBytes(x86reg::vectorReg(D));
              ASSERT_TRUE(Result);
              ASSERT_EQ(Result->size(), 64u);
              Words Actual{};
              std::memcpy(Actual.data(), Result->data(), 64);
              for (unsigned I = 0; I != 4; ++I)
                EXPECT_EQ(Actual[I], Expected[I]);
              for (unsigned I = 4; I != 8; ++I)
                EXPECT_EQ(Actual[I], 0u);
              for (const unsigned I : {S, W})
                if (I != D) {
                  const auto Source =
                      Emu.getRegisterBytes(x86reg::vectorReg(I));
                  ASSERT_TRUE(Source);
                  EXPECT_EQ(
                      std::memcmp(Source->data(), Registers[I].data(), 64), 0);
                }
              for (const auto F : {x86reg::CF, x86reg::ZF, x86reg::OF,
                                   x86reg::PF, x86reg::SF, x86reg::AF})
                EXPECT_EQ(Emu.getRegister(F), 1u);
            }
          }
      }
}

TEST(X86SHA512, InvalidWidthModifiersAndMismatchedEncodingAreRejected) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const uint8_t Bytes[] = {0xc4, 0xe2, 0x7f, 0xcc, 0xd3};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, I), sizeof(Bytes));
  ASSERT_NE(I.Raw, nullptr);
  ASSERT_NE(I.Raw->detail, nullptr);
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    cs_insn Raw = *I.Raw;
    cs_detail Detail = *Raw.detail;
    Raw.detail = &Detail;
    switch (Case) {
    case 0:
      Detail.x86.operands[1].size = 32;
      Detail.x86.operands[1].reg = X86_REG_YMM3;
      break;
    case 1:
      Detail.x86.avx_sae = true;
      break;
    case 2:
      Raw.bytes[2] |= 0x80; // VEX.W is reserved.
      break;
    case 3:
      Raw.bytes[4] = 0xd4; // Source register disagrees with decoded operands.
      break;
    }
    X86Lifter L(Arch::X64);
    L.setStrict(true);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

} // namespace

TEST(X86SHA512, Mode32IgnoresUsedRegisterExtensionFields) {
  const Words D = {0x0123456789abcdef, 0x8899aabbccddeeff, 0x1020304050607080,
                   0xfedcba9876543210};
  const Words S = {0xa1b2c3d4e5f60718, 0xf1e2d3c4b5a69788, 0x1122334455667788,
                   0x9988776655443322};
  const Words W = {0xffffffffffffffff, 0x55555555aaaaaaaa};
  for (const unsigned Opcode : {0xcc, 0xcd, 0xcb})
    for (const bool ExtendedB : {false, true})
      for (const bool ExtendedV : {false, true}) {
        if (Opcode != 0xcb && ExtendedV)
          continue; // unused vvvv is reserved
        SCOPED_TRACE(Opcode);
        SCOPED_TRACE(ExtendedB);
        SCOPED_TRACE(ExtendedV);
        const std::vector<uint8_t> Bytes = {
            0xc4, static_cast<uint8_t>(ExtendedB ? 0xc2 : 0xe2),
            static_cast<uint8_t>(Opcode == 0xcb ? (ExtendedV ? 0x27 : 0x67)
                                                : 0x7f),
            static_cast<uint8_t>(Opcode),
            static_cast<uint8_t>(Opcode == 0xcb ? 0xd4 : 0xd3)};
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::X86));
        DecodedInsn I{};
        ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
                  Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(Dec.liftToLow(I, Ops));
        BinaryImage Image;
        Image.Arch = Arch::X86;
        NdOpEmulator Emu(Image);
        for (const auto &[R, V] : std::array<std::pair<unsigned, Words>, 3>{
                 {{2, D}, {3, S}, {4, W}}})
          Emu.setRegisterBytes(
              x86reg::vectorReg(R),
              {reinterpret_cast<const uint8_t *>(V.data()), 64});
        for (const auto &Op : Ops)
          ASSERT_TRUE(Emu.step(Op));
        const auto Expected = sha512Reference(Opcode, D, S, W);
        const auto Actual = Emu.getRegisterBytes(x86reg::XMM2);
        ASSERT_TRUE(Actual);
        EXPECT_EQ(std::memcmp(Actual->data(), Expected.data(), 32), 0);
      }
}

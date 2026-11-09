//===- MIPSDSPTests.cpp - DSP state and packed byte semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent selector groups, lane results and original-machine state.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> encodeDSP(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  std::vector<uint8_t> Bytes;
  for (uint32_t Word : Words)
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(Word >> (8 * (archLittleEndian(Target) ? I : 3 - I)));
  return Bytes;
}

BinaryImage dspImage(Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Bitness::Bits32;
  Image.Entry = 0x10000;
  Image.MIPS.emplace();
  Image.MIPS->HasDSP = true;
  return Image;
}

std::vector<LowOp> liftDSP(Decoder &Decode, Arch Target, uint32_t Word) {
  const auto Bytes = encodeDSP({Word}, Target);
  DecodedInsn Insn;
  if (Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000, Insn) != 4)
    throw std::runtime_error("reference DSP encoding did not decode");
  std::vector<LowOp> Ops;
  Decode.liftToLow(Insn, Ops);
  return Ops;
}

bool executeDSP(Decoder &Decode, Arch Target, uint32_t Word,
                NdOpEmulator &Emulator) {
  for (const auto &Op : liftDSP(Decode, Target, Word)) {
    EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == mipsreg::Zero);
    if (!Emulator.step(Op))
      return false;
  }
  return true;
}

// Manual-derived masks deliberately do not include the production table.
uint32_t selectedFields(unsigned Selector) {
  const uint32_t Fields[] = {0x3f, 0x1f80, 0x2000, 0xff0000, 0xf000000, 0x4000};
  uint32_t Value = 0;
  for (unsigned I = 0; I != 6; ++I)
    if (Selector & (1u << I))
      Value |= Fields[I];
  return Value;
}

class MIPSDSP : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSDSP, SelectorsPreserveUnselectedFieldsAndIgnoreHighBits) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (unsigned Selector = 0; Selector != 64; ++Selector)
    for (unsigned Ignored : {0u, 0x3c0u}) {
      SCOPED_TRACE(Selector | Ignored);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(mipsreg::DSPControl, 0x052a51a5);
      Emulator.setRegister(mipsreg::A0, 0xfad5ae5a);
      ASSERT_TRUE(executeDSP(Decode, GetParam(),
                             0x7c8004f8 | ((Selector | Ignored) << 11),
                             Emulator)); // wrdsp a0,selector
      const uint32_t Mask = selectedFields(Selector);
      const uint32_t Expected =
          ((0x052a51a5 & ~Mask) | (0xfad5ae5a & Mask)) & 0x0fff7fbf;
      EXPECT_EQ(Emulator.getRegister(mipsreg::DSPControl), Expected);
      ASSERT_TRUE(executeDSP(Decode, GetParam(),
                             0x7c0014b8 | ((Selector | Ignored) << 16),
                             Emulator)); // rddsp v0,selector
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected & Mask);
    }
}

TEST_P(MIPSDSP, MissingStateIsNeverInventedAndFullWriteInitializesIt) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  NdOpEmulator Unknown(Image);
  Unknown.setStrictMode(true);
  EXPECT_FALSE(executeDSP(Decode, GetParam(), 0x7c3f14b8, Unknown));
  EXPECT_FALSE(Unknown.getRegister(mipsreg::DSPControl));
  EXPECT_FALSE(Unknown.getRegister(mipsreg::V0));
  Unknown.setRegister(mipsreg::A0, 0xffffffff);
  EXPECT_FALSE(executeDSP(Decode, GetParam(), 0x7c800cf8, Unknown));
  EXPECT_FALSE(Unknown.getRegister(mipsreg::DSPControl));
  NdOpEmulator Complete(Image);
  Complete.setStrictMode(true);
  Complete.setRegister(mipsreg::A0, 0xffffffff);
  ASSERT_TRUE(executeDSP(Decode, GetParam(), 0x7c81fcf8, Complete));
  EXPECT_EQ(Complete.getRegister(mipsreg::DSPControl), 0x0fff7fbfu);
  ASSERT_TRUE(executeDSP(Decode, GetParam(), 0x7c3f14b8, Complete));
  EXPECT_EQ(Complete.getRegister(mipsreg::V0), 0x0fff7fbfu);
  NdOpEmulator EmptyMask(Image);
  EmptyMask.setStrictMode(true);
  EXPECT_TRUE(executeDSP(Decode, GetParam(), 0x7c8004f8, EmptyMask));
  EXPECT_TRUE(executeDSP(Decode, GetParam(), 0x7c0014b8, EmptyMask));
  EXPECT_EQ(EmptyMask.getRegister(mipsreg::V0), 0u);
  EXPECT_FALSE(EmptyMask.getRegister(mipsreg::DSPControl));
}

TEST_P(MIPSDSP, PackedArithmeticPreservesStickyFlagsIncludingZeroDestination) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const std::array<std::array<uint32_t, 2>, 6> Inputs = {
      {{0xff01807f, 0x02018081},
       {0x11223344, 0x55667788},
       {0xffffffff, 1},
       {0x01000000, 0xff000000},
       {0, 0},
       {0x80808080, 0x80808080}}};
  for (const auto &Input : Inputs)
    for (bool Subtract : {false, true})
      for (bool Saturate : {false, true})
        for (unsigned Destination : {0u, 2u, 4u, 5u})
          for (uint32_t Initial : {0u, 0x01100021u}) {
            SCOPED_TRACE(Destination);
            NdOpEmulator Emulator(Image);
            Emulator.setStrictMode(true);
            Emulator.setRegister(mipsreg::A0, Input[0]);
            Emulator.setRegister(mipsreg::A1, Input[1]);
            Emulator.setRegister(mipsreg::DSPControl, Initial);
            uint32_t Result = 0, State = Initial;
            for (unsigned Byte = 0; Byte != 4; ++Byte) {
              const int A = (Input[0] >> (Byte * 8)) & 255;
              const int B = (Input[1] >> (Byte * 8)) & 255;
              const int Value = Subtract ? A - B : A + B;
              State |= Value < 0 || Value > 255 ? 0x100000 : 0;
              Result |=
                  uint32_t(Saturate ? std::clamp(Value, 0, 255) : Value & 255)
                  << (Byte * 8);
            }
            ASSERT_TRUE(executeDSP(Decode, GetParam(),
                                   0x7c850010 | (Destination << 11) |
                                       (Saturate ? 0x100 : 0) |
                                       (Subtract ? 0x40 : 0),
                                   Emulator));
            EXPECT_EQ(Emulator.getRegister(mipsreg::DSPControl), State);
            if (Destination)
              EXPECT_EQ(Emulator.getRegister(mipsreg::r(Destination)), Result);
            EXPECT_FALSE(Emulator.skips().any());
          }
}

TEST_P(MIPSDSP, SignedHalfwordsSaturateIndependentlyAndPreserveStickyState) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const uint32_t Inputs[] = {0,          0x7fff8000, 0x80007fff, 0xffffffff,
                             0x00010001, 0x80008000, 0x7fff7fff, 0x1234fedc};
  for (bool Subtract : {false, true})
    for (bool Saturate : {false, true})
      for (uint32_t Left : Inputs)
        for (uint32_t Right : Inputs)
          for (uint32_t Initial : {0u, 0x01000021u, 0x05557f9fu})
            for (unsigned Destination : {0u, 2u, 4u, 5u}) {
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              Emulator.setRegister(mipsreg::A0, Left);
              Emulator.setRegister(mipsreg::A1, Right);
              Emulator.setRegister(mipsreg::DSPControl, Initial);
              uint32_t Result = 0, State = Initial;
              for (unsigned Lane = 0; Lane != 2; ++Lane) {
                const int A = int16_t(Left >> (Lane * 16));
                const int B = int16_t(Right >> (Lane * 16));
                const int Value = Subtract ? A - B : A + B;
                if (Value < -32768 || Value > 32767)
                  State |= 0x100000;
                Result |=
                    uint32_t(uint16_t(
                        Saturate ? std::clamp(Value, -32768, 32767) : Value))
                    << (Lane * 16);
              }
              // MD00076/DSP manual selectors; deliberately no production table.
              ASSERT_TRUE(executeDSP(Decode, GetParam(),
                                     0x7c850290 | (Destination << 11) |
                                         (Subtract ? 0x40 : 0) |
                                         (Saturate ? 0x100 : 0),
                                     Emulator));
              EXPECT_EQ(Emulator.getRegister(mipsreg::DSPControl), State);
              if (Destination)
                EXPECT_EQ(Emulator.getRegister(mipsreg::r(Destination)),
                          Result);
              EXPECT_FALSE(Emulator.skips().any());
            }
}

TEST_P(MIPSDSP, DecoderReuseConsumesTheCurrentImagesFeatureEvidence) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  EXPECT_THROW(liftDSP(Decode, GetParam(), 0x7c3f14b8), UnliftedInstruction);
  for (bool Enabled : {true, false, true, false}) {
    Image.MIPS->HasDSP = Enabled;
    ASSERT_TRUE(Decode.selectMode(Image, Image.Entry));
    if (Enabled)
      EXPECT_NO_THROW(liftDSP(Decode, GetParam(), 0x7c3f14b8));
    else
      EXPECT_THROW(liftDSP(Decode, GetParam(), 0x7c3f14b8),
                   UnliftedInstruction);
  }
}

TEST_P(MIPSDSP, LiteralArithmeticAndStateMatchThePinnedEnabledCPU) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    static_cast<uc_mode>(UC_MODE_MIPS32 |
                                         (archLittleEndian(GetParam())
                                              ? UC_MODE_LITTLE_ENDIAN
                                              : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_74KF), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  // Enable Status.MX before executing the DSP sequence.
  const auto Code = encodeDSP({0x40086000, 0x3c090100, 0x01094025, 0x40886000,
                               0x000000c0, 0x7c81fcf8, 0x7ca61010, 0x7ca61910,
                               0x7ca64050, 0x7ca64950, 0x7c3f3cb8},
                              GetParam());
  ASSERT_EQ(uc_mem_write(Raw, 0x10000, Code.data(), Code.size()), UC_ERR_OK);
  uint32_t State = 0x01000021, A = 0xff01807f, B = 0x02018081;
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &State), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &A), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_6, &B), UC_ERR_OK);
  ASSERT_EQ(uc_emu_start(Raw, 0x10000, 0x10000 + Code.size(), 0, 32),
            UC_ERR_OK);
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::A0, State);
  Emulator.setRegister(mipsreg::A1, A);
  Emulator.setRegister(mipsreg::A2, B);
  for (uint32_t Word : {0x7c81fcf8u, 0x7ca61010u, 0x7ca61910u, 0x7ca64050u,
                        0x7ca64950u, 0x7c3f3cb8u})
    ASSERT_TRUE(executeDSP(Decode, GetParam(), Word, Emulator));
  for (unsigned Register : {2u, 3u, 7u, 8u, 9u}) {
    uint32_t Value = 0;
    ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Register, &Value), UC_ERR_OK);
    EXPECT_EQ(Emulator.getRegister(mipsreg::r(Register)), Value);
  }
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 0x01020000u);
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), 0xff02ffffu);
  EXPECT_EQ(Emulator.getRegister(mipsreg::r(8)), 0xfd0000feu);
  EXPECT_EQ(Emulator.getRegister(mipsreg::r(9)), 0xfd000000u);
  EXPECT_EQ(Emulator.getRegister(mipsreg::DSPControl), 0x01100021u);
  // Unicorn currently drops arithmetic with rd=zero before its OUFLAG write.
  // That case is qualified by the independent manual expectation above.
#endif
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSDSP,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace

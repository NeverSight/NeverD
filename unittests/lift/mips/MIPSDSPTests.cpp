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
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
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

struct PureCase {
  const char *Name;
  uint32_t Word, Result;
  unsigned Source;
};
constexpr PureCase PureCases[] = {
#define MIPS_DSP_PURE(Name, Word, Result, Source) {#Name, Word, Result, Source},
#include "MIPSDSPExpandFixtures.def"
};
TEST_P(MIPSDSP, ExpansionReplicationAndReductionHaveNoControlDependency) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (const auto &C : PureCases)
    for (unsigned Destination : {0u, 2u, 4u, 31u}) {
      SCOPED_TRACE(C.Name);
      const auto Word = (C.Word & ~0xf800u) | (Destination << 11);
      for (bool Seed : {false, true}) {
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        if (Seed)
          E.setRegister(mipsreg::DSPControl, 0x05ab51a5);
        if (Destination && C.Source)
          E.setRegister(mipsreg::r(C.Source), 0x89abcdef);
        ASSERT_TRUE(executeDSP(Decode, GetParam(), Word, E));
        if (Destination)
          EXPECT_EQ(E.getRegister(mipsreg::r(Destination)), C.Result);
        if (Seed)
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x05ab51a5u);
        else
          EXPECT_FALSE(E.getRegister(mipsreg::DSPControl));
      }
      if (Destination && C.Source) {
        NdOpEmulator Missing(Image);
        Missing.setStrictMode(true);
        if (Destination != C.Source)
          Missing.setRegister(mipsreg::r(Destination), 99);
        EXPECT_FALSE(executeDSP(Decode, GetParam(), Word, Missing));
        if (Destination != C.Source)
          EXPECT_EQ(Missing.getRegister(mipsreg::r(Destination)), 99u);
      }
    }
  for (unsigned I = 0; I < 1024; ++I) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    ASSERT_TRUE(executeDSP(Decode, GetParam(), 0x7c001292 | (I << 16), E));
    const uint32_t V = uint16_t(I < 512 ? int(I) : int(I) - 1024);
    EXPECT_EQ(E.getRegister(mipsreg::V0), V | (V << 16));
  }
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, 0xffffffff);
  ASSERT_TRUE(executeDSP(Decode, GetParam(), 0x7c801510, E));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 1020u);
}
TEST_P(MIPSDSP, ExpansionReservedFieldsAndFeatureEvidenceAreRequired) {
  MIPSLifter Lift(GetParam());
  Lift.setDSPEnabled(true);
  for (uint32_t Word : {0x7c241312u, 0x7c811510u, 0x7da51092u, 0x7c2410d2u}) {
    cs_insn I{};
    I.size = 4;
    I.address = 0x10000;
    auto B = encodeDSP({Word}, GetParam());
    std::copy(B.begin(), B.end(), I.bytes);
    std::vector<LowOp> Ops;
    EXPECT_THROW(Lift.lift(I, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  for (const auto &C : PureCases)
    EXPECT_THROW(liftDSP(Decode, GetParam(), C.Word), UnliftedInstruction);
}
TEST_P(MIPSDSP, ExpansionLiteralsMatchPinnedTargetAndPreserveControl) {
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
  std::unique_ptr<uc_engine, decltype(&uc_close)> E(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_74KF), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
  auto Setup = encodeDSP(
      {0x40086000, 0x3c090100, 0x01094025, 0x40886000, 0xc0, 0x7c81fcf8},
      GetParam());
  uint32_t Control = 0x05ab51a5;
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_A0, &Control), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(Raw, 0x10000, Setup.data(), Setup.size()), UC_ERR_OK);
  ASSERT_EQ(
      uc_emu_start(Raw, 0x10000, 0x10000 + Setup.size(), 0, Setup.size() / 4),
      UC_ERR_OK);
  unsigned Index = 0;
  for (const auto &C : PureCases)
    for (unsigned Destination : {0u, 2u, 4u, 31u}) {
      SCOPED_TRACE(C.Name);
      uint32_t Input = 0x89abcdef;
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_A0, &Input), UC_ERR_OK);
      auto Code =
          encodeDSP({(C.Word & ~0xf800u) | (Destination << 11), 0x7c3f4cb8},
                    GetParam()); // rddsp t1,63
      uint64_t PC = 0x10100 + 8 * Index++;
      ASSERT_EQ(uc_mem_write(Raw, PC, Code.data(), Code.size()), UC_ERR_OK);
      ASSERT_EQ(uc_emu_start(Raw, PC, PC + Code.size(), 0, 2), UC_ERR_OK);
      uint32_t Value = 0;
      if (Destination) {
        ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Destination, &Value),
                  UC_ERR_OK);
        EXPECT_EQ(Value, C.Result);
      }
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_T1, &Value), UC_ERR_OK);
      EXPECT_EQ(Value, Control);
    }
#endif
}

enum class WordOperation {
#define MIPS_DSP_WORD(Name, Word) Name,
#include "MIPSDSPWordFixtures.def"
};
TEST_P(MIPSDSP, WordCarrySaturationPredicatesAndPicksPreserveOtherState) {
  const struct {
    WordOperation Kind;
    uint32_t Word;
  } Cases[] = {
#define MIPS_DSP_WORD(Name, Word) {WordOperation::Name, Word},
#include "MIPSDSPWordFixtures.def"
  };
  auto I = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  const auto Signed = [](uint32_t V) -> int64_t {
    return V & 0x80000000 ? int64_t(V) - (INT64_C(1) << 32) : V;
  };
  for (const auto &C : Cases)
    for (unsigned RD : {0u, 2u, 4u, 5u, 31u})
      for (uint32_t A :
           {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x01234567u})
        for (uint32_t B :
             {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x89abcdefu})
          for (uint32_t Control : {0u, 0x0fff7fbfu, 0x05ab51a5u, 0x0a002000u}) {
            const bool Compare = C.Kind >= WordOperation::Equal &&
                                 C.Kind <= WordOperation::LessEqual;
            const bool Pure = C.Kind == WordOperation::ModSub ||
                              C.Kind >= WordOperation::PickBytes;
            uint32_t Expected = 0, State = Control;
            if (C.Kind == WordOperation::AddCarry) {
              uint64_t Sum = uint64_t(A) + B;
              Expected = Sum;
              State = (State & ~0x2000u) | ((Sum >> 32) << 13);
            } else if (C.Kind <= WordOperation::UseCarry) {
              int64_t Sum = C.Kind == WordOperation::SubSat
                                ? Signed(A) - Signed(B)
                                : Signed(A) + Signed(B);
              if (C.Kind == WordOperation::UseCarry)
                Sum += (Control >> 13) & 1;
              if (Sum < INT32_MIN || Sum > INT32_MAX)
                State |= 0x100000;
              Expected = C.Kind == WordOperation::UseCarry
                             ? uint32_t(Sum)
                             : uint32_t(std::clamp<int64_t>(Sum, INT32_MIN,
                                                            INT32_MAX));
            } else if (C.Kind == WordOperation::ModSub) {
              Expected = A ? A - (B & 255) : (B >> 8) & 65535;
            } else {
              const unsigned Bits =
                  C.Kind == WordOperation::PickHalves ? 16 : 8;
              if (Compare)
                State &= ~0x0f000000u;
              for (unsigned Lane = 0; Lane < 32 / Bits; ++Lane) {
                const unsigned Mask = (1u << Bits) - 1;
                const auto X = (A >> (Lane * Bits)) & Mask,
                           Y = (B >> (Lane * Bits)) & Mask;
                if (Compare) {
                  const bool Test = C.Kind == WordOperation::Equal  ? X == Y
                                    : C.Kind == WordOperation::Less ? X < Y
                                                                    : X <= Y;
                  State |= uint32_t(Test) << (24 + Lane);
                } else
                  Expected |= ((Control >> (24 + Lane)) & 1 ? X : Y)
                              << (Lane * Bits);
              }
            }
            NdOpEmulator E(I);
            E.setStrictMode(true);
            E.setRegister(mipsreg::A0, A);
            E.setRegister(mipsreg::A1, B);
            E.setRegister(mipsreg::DSPControl, Control);
            ASSERT_TRUE(executeDSP(D, GetParam(),
                                   C.Word | (Compare ? 0 : RD << 11), E));
            if (!Compare && RD)
              EXPECT_EQ(E.getRegister(mipsreg::r(RD)), Expected);
            EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                      Pure ? Control : State);
          }
  for (const auto &C : Cases) {
    const bool Compare =
        C.Kind >= WordOperation::Equal && C.Kind <= WordOperation::LessEqual;
    const auto Word = C.Word | (Compare ? 0 : 2 << 11);
    for (unsigned Missing : {mipsreg::A0, mipsreg::A1, mipsreg::DSPControl}) {
      NdOpEmulator E(I);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 99);
      if (Missing != mipsreg::A0)
        E.setRegister(mipsreg::A0, 7);
      if (Missing != mipsreg::A1)
        E.setRegister(mipsreg::A1, 3);
      if (Missing != mipsreg::DSPControl)
        E.setRegister(mipsreg::DSPControl, 0x05ab51a5);
      const bool NoControlNeeded =
          C.Kind == WordOperation::ModSub && Missing == mipsreg::DSPControl;
      EXPECT_EQ(executeDSP(D, GetParam(), Word, E), NoControlNeeded);
      if (!NoControlNeeded)
        EXPECT_EQ(E.getRegister(mipsreg::V0), 99u);
      if (Missing != mipsreg::DSPControl)
        EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x05ab51a5u);
    }
  }
}

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

TEST_P(MIPSDSP, RightShiftAndPackingLeaveControlUntouched) {
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  struct Case {
    uint32_t Word;
    unsigned Bytes;
    bool Variable, Rounded;
  };
  constexpr Case Cases[] = {
#define MIPS_DSP_SHIFT(Word, Bytes, Variable, Rounded)                         \
  {Word, Bytes, Variable, Rounded},
#include "MIPSDSPShiftFixtures.def"
  };
  for (const auto &C : Cases)
    for (uint32_t Input : {0u, 0x7fff7fffu, 0x80008000u, 0xffffffffu,
                           0xfffffffdu, 0x7fffffffu, 0x80000000u, 0x1234abcdu})
      for (unsigned Count = 0; Count != (C.Variable ? 64 : C.Bytes * 8);
           ++Count)
        for (unsigned Destination : {0u, 2u, 4u, 5u})
          for (bool KnownControl : {false, true}) {
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            if (KnownControl)
              E.setRegister(mipsreg::DSPControl, 0x0a555fa5);
            if (Destination) {
              E.setRegister(mipsreg::A0, 0x80000000u | Count);
              E.setRegister(mipsreg::A1, Input);
            }
            uint32_t Expected = 0;
            const unsigned Bits = C.Bytes * 8, Amount = Count & (Bits - 1);
            for (unsigned I = 0; I != 32 / Bits; ++I) {
              const uint32_t Raw = Input >> (I * Bits);
              int64_t Value = Bits == 8    ? int64_t(uint8_t(Raw))
                              : Bits == 16 ? int64_t(int16_t(Raw))
                                           : int64_t(int32_t(Raw));
              if (C.Rounded && Amount)
                Value += int64_t(1) << (Amount - 1);
              const uint32_t Mask = Bits == 32 ? 0xffffffff : (1u << Bits) - 1;
              Expected |= (uint32_t(Value >> Amount) & Mask) << (I * Bits);
            }
            const uint32_t Word = C.Word | ((C.Variable ? 4 : Count) << 21) |
                                  (5 << 16) | (Destination << 11);
            SCOPED_TRACE(::testing::Message() << Word << ": " << Input);
            ASSERT_TRUE(executeDSP(Decode, GetParam(), Word, E));
            if (Destination)
              EXPECT_EQ(E.getRegister(mipsreg::r(Destination)), Expected);
            EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                      KnownControl ? std::optional<uint64_t>(0x0a555fa5)
                                   : std::nullopt);
          }
  for (bool Halfwords : {false, true})
    for (unsigned Destination : {0u, 2u, 4u, 5u}) {
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      if (Destination) {
        E.setRegister(mipsreg::A0, 0x11223344);
        E.setRegister(mipsreg::A1, 0xaabbccdd);
      }
      ASSERT_TRUE(executeDSP(
          Decode, GetParam(),
          (Halfwords ? 0x7c850511 : 0x7c850311) | (Destination << 11), E));
      if (Destination)
        EXPECT_EQ(E.getRegister(mipsreg::r(Destination)),
                  Halfwords ? 0x1122aabbu : 0x1133aaccu);
      EXPECT_FALSE(E.getRegister(mipsreg::DSPControl));
    }
}

TEST_P(MIPSDSP, PureFormatsStillValidateFeatureFieldsAndInputState) {
  MIPSLifter Lifter(GetParam());
  Lifter.setDSPEnabled(true);
  for (uint32_t Word : {0x7d000053u, 0x7e000053u, 0x7e000253u, 0x7e000353u}) {
    const auto Bytes = encodeDSP({Word}, GetParam());
    cs_insn Insn{};
    Insn.address = 0x10000;
    Insn.size = 4;
    std::copy(Bytes.begin(), Bytes.end(), Insn.bytes);
    std::vector<LowOp> Ops(1);
    EXPECT_THROW(Lifter.lift(Insn, Ops), UnliftedInstruction);
    EXPECT_EQ(Ops.size(), 1u);
  }
  auto Image = dspImage(GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (uint32_t Word : {0x7c8510d3u, 0x7c8515d3u, 0x7c851311u, 0x7c851511u}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::V0, 7);
    EXPECT_FALSE(executeDSP(Decode, GetParam(), Word, E));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 7u);
    ASSERT_TRUE(Decode.init(GetParam()));
    EXPECT_THROW(liftDSP(Decode, GetParam(), Word), UnliftedInstruction);
    ASSERT_TRUE(Decode.init(Image));
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

TEST_P(MIPSDSP, LeftShiftEveryCountSaturatesAndSetsOnlyStickyBit22) {
  const struct {
    const char *Name;
    uint32_t Encoding;
    unsigned Bytes;
    bool Variable, Saturated;
  } Cases[] = {
#define MIPS_DSP_LEFT(Name, Encoding, Bytes, Variable, Saturated)              \
  {#Name, Encoding, Bytes, Variable, Saturated},
#include "MIPSDSPLeftFixtures.def"
  };
  auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  for (const auto &C : Cases)
    for (unsigned Count = 0; Count != C.Bytes * 8; ++Count)
      for (uint32_t Input : {0u, 1u, 0xffffffffu, 0x80000000u, 0x7fffffffu,
                             0x80007fffu, 0x00018000u, 0x89abcdefu})
        for (unsigned RD : {0u, 2u, 4u, 5u}) {
          SCOPED_TRACE(::testing::Message()
                       << C.Name << " count=" << Count << " input=" << Input
                       << " rd=" << RD);
          const unsigned Bits = C.Bytes * 8;
          const uint64_t Modulus = UINT64_C(1) << Bits, Mask = Modulus - 1;
          uint32_t Result = 0;
          bool Overflow = false;
          for (unsigned I = 0; I != 32 / Bits; ++I) {
            const uint64_t Lane = (Input >> (I * Bits)) & Mask;
            const int64_t Value = C.Bytes == 1 || Lane < Modulus / 2
                                      ? int64_t(Lane)
                                      : int64_t(Lane) - int64_t(Modulus);
            int64_t Shifted = Value * (INT64_C(1) << Count);
            const int64_t Min = C.Bytes == 1 ? 0 : -int64_t(Modulus / 2);
            const int64_t Max =
                C.Bytes == 1 ? int64_t(Mask) : int64_t(Modulus / 2 - 1);
            Overflow |= Shifted < Min || Shifted > Max;
            if (C.Saturated)
              Shifted = std::clamp(Shifted, Min, Max);
            Result |= uint32_t(uint64_t(Shifted) & Mask) << (I * Bits);
          }
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          const uint32_t Control = Count & 1 ? 0x05ab51a5 : 0;
          E.setRegister(mipsreg::DSPControl, Control);
          E.setRegister(mipsreg::A0, Input);
          E.setRegister(mipsreg::A1, 0xffffffe0u | Count);
          const uint32_t Word = C.Encoding | (RD << 11) | (4 << 16) |
                                ((C.Variable ? 5 : Count) << 21);
          ASSERT_TRUE(executeDSP(D, GetParam(), Word, E));
          if (RD)
            EXPECT_EQ(E.getRegister(mipsreg::r(RD)), Result);
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                    Control | (Overflow ? 1u << 22 : 0));
        }
  for (uint32_t Word : {0x7c041013u, 0x7c041213u, 0x7c041313u, 0x7c041513u}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 0);
    EXPECT_FALSE(executeDSP(D, GetParam(), Word, E));
  }
  MIPSLifter L(GetParam());
  L.setDSPEnabled(true);
  for (uint32_t Word : {0x7d041013u, 0x7e041213u, 0x7e041313u}) {
    const auto Bytes = encodeDSP({Word}, GetParam());
    cs_insn Raw{};
    Raw.size = 4;
    std::copy(Bytes.begin(), Bytes.end(), Raw.bytes);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(Raw, Ops), std::runtime_error);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST_P(MIPSDSP, SaturatedAbsoluteAndPureGPRPredicatesHaveDistinctStateEffects) {
  auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  for (unsigned Bytes : {2u, 4u})
    for (uint32_t Input : {0u, 1u, 0xffffffffu, 0x80000000u, 0x80008000u,
                           0x7ffffffeu, 0x89abcdefu})
      for (unsigned RD : {0u, 2u, 4u}) {
        const unsigned Bits = Bytes * 8;
        const uint64_t Modulus = UINT64_C(1) << Bits;
        uint32_t Result = 0;
        bool Overflow = false;
        for (unsigned I = 0; I != 32 / Bits; ++I) {
          uint64_t Lane = (Input >> (I * Bits)) & (Modulus - 1);
          const uint64_t Magnitude =
              Lane & (Modulus / 2) ? Modulus - Lane : Lane;
          Overflow |= Magnitude == Modulus / 2;
          Result |= uint32_t(std::min(Magnitude, Modulus / 2 - 1))
                    << (I * Bits);
        }
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::DSPControl, 0x054b51a5);
        E.setRegister(mipsreg::A0, Input);
        ASSERT_TRUE(executeDSP(D, GetParam(),
                               (Bytes == 2 ? 0x7c000252 : 0x7c000452) |
                                   (4 << 16) | (RD << 11),
                               E));
        if (RD)
          EXPECT_EQ(E.getRegister(mipsreg::r(RD)), Result);
        EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                  0x054b51a5u | (Overflow ? 1u << 20 : 0));
      }
  for (unsigned Kind = 0; Kind < 3; ++Kind)
    for (uint32_t A : {0u, 0xffffffffu, 0x80ff007fu, 0x12345678u})
      for (uint32_t B : {0u, 0xffffffffu, 0x807fff00u, 0x12345678u})
        for (unsigned RD : {0u, 2u, 4u, 5u}) {
          unsigned Result = 0;
          for (unsigned I = 0; I != 4; ++I) {
            const auto X = uint8_t(A >> (8 * I)), Y = uint8_t(B >> (8 * I));
            Result |= unsigned(Kind == 0   ? X == Y
                               : Kind == 1 ? X < Y
                                           : X <= Y)
                      << I;
          }
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(mipsreg::A0, A);
          E.setRegister(mipsreg::A1, B);
          ASSERT_TRUE(executeDSP(D, GetParam(),
                                 0x7c850111 | (Kind << 6) | (RD << 11), E));
          if (RD)
            EXPECT_EQ(E.getRegister(mipsreg::r(RD)), Result);
        }
}

TEST_P(MIPSDSP,
       PackedMultipliesSaturateAndInvalidateAc0EvenWithZeroDestination) {
  auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  constexpr uint32_t Inputs[] = {0,          1,          0xffffffff, 0x80008000,
                                 0x7fff7fff, 0xff00ff00, 0x00ff00ff, 0x80017fff,
                                 0x0001ffff, 0xc0004000, 0x11223344, 0x40004000,
                                 0x01000200, 0x80808080, 0x01010101};
  for (uint32_t Encoding : {0x7c000190u, 0x7c0001d0u, 0x7c0007d0u})
    for (uint32_t A : Inputs)
      for (uint32_t B : Inputs)
        for (unsigned Destination : {0u, 2u, 4u, 5u})
          for (uint32_t Control : {0x050051a5u, 0x052051a5u}) {
            SCOPED_TRACE(::testing::Message()
                         << std::hex << Encoding << ": " << A << ", " << B);
            uint32_t Value = 0, State = Control;
            for (unsigned I = 0; I != 2; ++I) {
              int64_t Product;
              const auto Y = uint16_t(B >> (I * 16));
              if (Encoding == 0x7c0007d0) {
                const auto X = uint16_t(A >> (I * 16));
                if (X == 0x8000 && Y == 0x8000) {
                  Product = 32767;
                  State |= 1u << 21;
                } else {
                  const int64_t SX = X < 32768 ? X : int(X) - 65536;
                  const int64_t SY = Y < 32768 ? Y : int(Y) - 65536;
                  // Floor division with a positive divisor, independent of
                  // the lifter's arithmetic shift implementation.
                  const int64_t Rounded = SX * SY + 16384;
                  Product =
                      Rounded / 32768 - (Rounded < 0 && Rounded % 32768 != 0);
                }
              } else {
                const auto X =
                    uint8_t(A >> (8 * (I + (Encoding == 0x7c000190 ? 2 : 0))));
                Product = int64_t(X) * Y;
                if (Product > 65535) {
                  Product = 65535;
                  State |= 1u << 21;
                }
              }
              Value |= uint32_t(uint16_t(Product)) << (16 * I);
            }
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            E.setRegister(mipsreg::A0, A);
            E.setRegister(mipsreg::A1, B);
            E.setRegister(mipsreg::HI, 0x12345678);
            E.setRegister(mipsreg::LO, 0x87654321);
            E.setRegister(mipsreg::DSPControl, Control);
            ASSERT_TRUE(executeDSP(
                D, GetParam(), Encoding | 0x00850000 | (Destination << 11), E));
            if (Destination)
              EXPECT_EQ(E.getRegister(mipsreg::r(Destination)), Value);
            EXPECT_EQ(E.getRegister(mipsreg::DSPControl), State);
            EXPECT_FALSE(E.getRegister(mipsreg::HI));
            EXPECT_FALSE(E.getRegister(mipsreg::LO));
            EXPECT_FALSE(E.skips().any());
          }
  for (unsigned Missing : {mipsreg::A0, mipsreg::A1, mipsreg::DSPControl}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    for (unsigned R : {mipsreg::A0, mipsreg::A1, mipsreg::DSPControl})
      if (R != Missing)
        E.setRegister(R, 0);
    E.setRegister(mipsreg::V0, 42);
    E.setRegister(mipsreg::HI, 13);
    E.setRegister(mipsreg::LO, 17);
    EXPECT_FALSE(executeDSP(D, GetParam(), 0x7c8517d0, E));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 42u);
    EXPECT_EQ(E.getRegister(mipsreg::HI), 13u);
    EXPECT_EQ(E.getRegister(mipsreg::LO), 17u);
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSDSP,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace

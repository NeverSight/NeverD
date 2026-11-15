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
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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
  Image.Bits = isMIPS64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Image.Entry = 0x10000;
  Image.MIPS.emplace();
  Image.MIPS->ABI = isMIPS64(Target) ? MIPSABI::N64 : MIPSABI::O32;
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
class MIPSDSPRegisterWidth : public ::testing::TestWithParam<Arch> {};
TEST(MIPSDSP64, PackedGPRsMatchOriginalInstructionsAndFullControl) {
  struct Case {
    const char *Mnemonic;
    uint32_t Word;
  };
  constexpr Case Cases[] = {
#define NEVERD_TEST_DSP64(Name, Word) {Name, Word},
#include "MIPSDSP64Fixtures.def"
  };
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    D.setText(true);
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
    uc_engine *Raw = nullptr;
    ASSERT_EQ(
        uc_open(UC_ARCH_MIPS,
                uc_mode(UC_MODE_MIPS64 |
                        (archLittleEndian(Target) ? 0 : UC_MODE_BIG_ENDIAN)),
                &Raw),
        UC_ERR_OK);
    std::unique_ptr<uc_engine, decltype(&uc_close)> CPU(Raw, uc_close);
    ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS64_MIPS64DSPR2), UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
    const auto Install = [&](uint64_t PC, llvm::ArrayRef<uint32_t> Words) {
      auto Code = encodeDSP(Words, Target);
      ASSERT_EQ(uc_mem_write(Raw, PC, Code.data(), Code.size()), UC_ERR_OK);
    };
    Install(0x10000, {0x40086000, 0x3c090100, 0x01094025, 0x40886000, 0xc0});
    ASSERT_EQ(uc_emu_start(Raw, 0x10000, 0x10014, 0, 5), UC_ERR_OK);
#endif
    unsigned Row = 0;
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Mnemonic);
      auto Code = encodeDSP({C.Word}, Target);
      DecodedInsn Insn;
      ASSERT_EQ(D.decodeOne(Code.data(), Code.size(), 0x10000, Insn), 4);
      EXPECT_STREQ(Insn.Raw->mnemonic, C.Mnemonic);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
      ASSERT_FALSE(Ops.empty());
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
      const uint64_t PC = 0x10040 + 12 * Row;
      // Write DSPControl from a2, execute the original, and read it via rddsp.
      Install(PC, {0x7cc1fcf8, C.Word, 0x7c3f44b8});
      for (uint64_t A :
           {0ull, 0xffff800000017fffull, 0x80706050a0b0c0d0ull, ~0ull}) {
        for (uint64_t B :
             {0x80007fff80007fffull, 0x0123456789abcdefull, ~0ull}) {
          uint64_t Control = 0xa52140c0, Out = 0x123456789abcdef0;
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(mipsreg::DSPControl, Control);
          E.setRegister(mipsreg::A0, A);
          E.setRegister(mipsreg::A1, B);
          E.setRegister(mipsreg::V0, Out);
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &A), UC_ERR_OK);
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &B), UC_ERR_OK);
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_6, &Control), UC_ERR_OK);
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_2, &Out), UC_ERR_OK);
          ASSERT_EQ(uc_emu_start(Raw, PC, PC + 12, 0, 3), UC_ERR_OK);
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_2, &Out), UC_ERR_OK);
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_8, &Control), UC_ERR_OK);
          // The pinned Unicorn helper dispatches OB add/sub over its four
          // halfwords. Use an independent eight-lane integer oracle for these
          // four results and their sticky OUflag20.
          const unsigned Minor = C.Word & 0x7ff;
          if (Minor == 0x14 || Minor == 0x54 || Minor == 0x114 ||
              Minor == 0x154) {
            Out = 0;
            Control = 0xa52140c0 & 0xffff7fff;
            for (unsigned Lane = 0; Lane < 8; ++Lane) {
              const int X = uint8_t(A >> (8 * Lane)),
                        Y = uint8_t(B >> (8 * Lane));
              const int V = (Minor & 0x40) ? X - Y : X + Y;
              if (V < 0 || V > 255)
                Control |= 1u << 20;
              Out |=
                  uint64_t(uint8_t((Minor & 0x100) ? std::clamp(V, 0, 255) : V))
                  << (8 * Lane);
            }
          }
          for (const auto &Op : Ops)
            ASSERT_TRUE(E.step(Op)) << ndOpName(Op.Opcode);
          EXPECT_EQ(E.getRegister(mipsreg::V0), Out);
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl), uint32_t(Control));
        }
      }
#endif
      ++Row;
    }
  }
}

TEST(MIPSDSP64, PackedDotsAndSingleLanesUpdateTheSelected128BitAccumulator) {
  // Distinct positive halfwords make left/right selections and alternating
  // subtraction observable. Fixed instruction words use GNU as $ac0,$4,$5.
  constexpr int64_t Left = 2ll * 0x00010002 * 0x00050006;
  constexpr int64_t Right = 2ll * 0x00030004 * 0x00070008;
  struct Row {
    uint32_t Word;
    const char *Name;
    int64_t Delta;
  };
  const Row Rows[] = {{0x7c8500f4, "dpau.h.obl", 17},
                      {0x7c8501f4, "dpau.h.obr", 53},
                      {0x7c8502f4, "dpsu.h.obl", -17},
                      {0x7c8503f4, "dpsu.h.obr", -53},
                      {0x7c850134, "dpaq_s.w.qh", 140},
                      {0x7c850174, "dpsq_s.w.qh", -140},
                      {0x7c8501b4, "mulsaq_s.w.qh", -36},
                      {0x7c850334, "dpaq_sa.l.pw", Left + Right},
                      {0x7c850374, "dpsq_sa.l.pw", -Left - Right},
                      {0x7c8503b4, "mulsaq_s.l.pw", Left - Right},
                      {0x7c850434, "maq_sa.w.qhll", 10},
                      {0x7c850474, "maq_sa.w.qhlr", 24},
                      {0x7c8504b4, "maq_sa.w.qhrl", 42},
                      {0x7c8504f4, "maq_sa.w.qhrr", 64},
                      {0x7c850534, "maq_s.w.qhll", 10},
                      {0x7c850574, "maq_s.w.qhlr", 24},
                      {0x7c8505b4, "maq_s.w.qhrl", 42},
                      {0x7c8505f4, "maq_s.w.qhrr", 64},
                      {0x7c850734, "maq_s.l.pwl", Left},
                      {0x7c8507b4, "maq_s.l.pwr", Right}};
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    D.setText(true);
    for (const Row &R : Rows)
      for (unsigned Bank = 0; Bank < 4; ++Bank) {
        SCOPED_TRACE(::testing::Message() << R.Name << " bank " << Bank);
        const auto Word = R.Word | Bank << 11;
        const auto B = encodeDSP({Word}, Target);
        DecodedInsn I;
        ASSERT_EQ(D.decodeOne(B.data(), B.size(), 0x10000, I), 4);
        EXPECT_STREQ(I.Raw->mnemonic, R.Name);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, 0x0001000200030004);
        E.setRegister(mipsreg::A1, 0x0005000600070008);
        E.setRegister(mipsreg::DSPControl, 0xa5010003);
        for (unsigned N = 0; N < 4; ++N) {
          E.setRegister(mipsreg::accumulatorHI(N), N == Bank ? 0 : 0x1234);
          E.setRegister(mipsreg::accumulatorLO(N), N == Bank ? 5 : 0x5678);
        }
        ASSERT_TRUE(executeDSP(D, Target, Word, E));
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank)),
                  uint64_t(5 + R.Delta));
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(Bank)),
                  5 + R.Delta < 0 ? UINT64_MAX : 0);
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(Bank ^ 1)), 0x1234u);
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank ^ 1)), 0x5678u);
        EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0xa5010003u);
      }
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    auto HI = mipsreg::accumulatorHI(2), LO = mipsreg::accumulatorLO(2);
    E.setRegister(mipsreg::DSPControl, 0x23);
    E.setRegister(mipsreg::A0, 1);
    E.setRegister(mipsreg::A1, 2);
    E.setRegister(HI, 0x123456789abcdef0ull);
    E.setRegister(LO, UINT64_MAX);
    ASSERT_TRUE(executeDSP(D, Target, 0x7c8511f4, E)); // dpau.h.obr ac2
    EXPECT_EQ(E.getRegister(HI), 0x123456789abcdef1ull);
    EXPECT_EQ(E.getRegister(LO), 1u);
    E.setRegister(HI, 0);
    E.setRegister(LO, INT64_MAX);
    ASSERT_TRUE(executeDSP(D, Target, 0x7c851334, E)); // dpaq_sa.l.pw ac2
    EXPECT_EQ(E.getRegister(HI), 0u);
    EXPECT_EQ(E.getRegister(LO), uint64_t(INT64_MAX));
    EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x40023u);
    E.setRegister(mipsreg::A0, 0x8000);
    E.setRegister(mipsreg::A1, 0x8000);
    E.setRegister(mipsreg::DSPControl, 0);
    E.setRegister(HI, 0);
    E.setRegister(LO, 0);
    ASSERT_TRUE(executeDSP(D, Target, 0x7c8515f4, E)); // maq_s.w.qhrr ac2
    EXPECT_EQ(E.getRegister(LO), uint64_t(INT32_MAX));
    EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x40000u);
  }
}

TEST(MIPSDSP64, UnsignedWordProductsAccumulateAcrossAll128Bits) {
  constexpr uint64_t Inputs[][2] = {{0, UINT64_MAX},
                                    {0x10000, 0x10000},
                                    {0xffffffff, 0xffffffff},
                                    {0xffffffff00000000, 0xffffffff},
                                    {UINT64_MAX, UINT64_MAX},
                                    {0x0001000000000011, 0x0001000000000003},
                                    {0xaaaaaaaa00000005, 0x5555555500000007}};
  constexpr uint64_t Seeds[][2] = {
      {0, 0}, {0, UINT64_MAX}, {UINT64_MAX, UINT64_MAX}, {0x1234, 1}};
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    D.setText(true);
    for (bool Subtract : {false, true})
      for (unsigned Bank = 0; Bank < 4; ++Bank) {
        const uint32_t Word =
            (Subtract ? 0x7c8507f4 : 0x7c850774) | (Bank << 11);
        auto Bytes = encodeDSP({Word}, Target);
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOne(Bytes.data(), Bytes.size(), 0x10000, Insn), 4);
        EXPECT_STREQ(Insn.Raw->mnemonic, Subtract ? "dmsubu" : "dmaddu");
        for (const auto &XY : Inputs)
          for (const auto &Seed : Seeds) {
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            E.setRegister(mipsreg::A0, XY[0]);
            E.setRegister(mipsreg::A1, XY[1]);
            E.setRegister(mipsreg::DSPControl, 0x12345678);
            E.setRegister(mipsreg::AC0Pending, 1);
            for (unsigned B = 0; B < 4; ++B) {
              E.setRegister(mipsreg::accumulatorHI(B),
                            B == Bank ? Seed[0] : 11);
              E.setRegister(mipsreg::accumulatorLO(B),
                            B == Bank ? Seed[1] : 22);
            }
            ASSERT_TRUE(executeDSP(D, Target, Word, E));
            const auto Product = [&](unsigned Shift) {
              return llvm::APInt(128, uint32_t(XY[0] >> Shift)) *
                     llvm::APInt(128, uint32_t(XY[1] >> Shift));
            };
            const auto Dot = Product(0) + Product(32);
            const auto Initial =
                llvm::APInt(64, Seed[0]).concat(llvm::APInt(64, Seed[1]));
            const auto Expected = Subtract ? Initial - Dot : Initial + Dot;
            for (unsigned B = 0; B < 4; ++B) {
              EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(B)),
                        B == Bank ? Expected.extractBitsAsZExtValue(64, 64)
                                  : 11u);
              EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(B)),
                        B == Bank ? Expected.extractBitsAsZExtValue(64, 0)
                                  : 22u);
            }
            EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x12345678u);
            EXPECT_EQ(E.getRegister(mipsreg::AC0Pending), unsigned(Bank != 0));
          }
        const auto Narrow =
            archLittleEndian(Target) ? Arch::MIPS32EL : Arch::MIPS32;
        for (auto Profile : {Target, Narrow}) {
          MIPSLifter L(Profile);
          L.setDSPEnabled(Profile == Narrow);
          std::vector<LowOp> Ops;
          EXPECT_THROW(L.lift(*Insn.Raw, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
  }
}

TEST(MIPSDSP64, ExtractsBothAccumulatorHalvesWithoutHostShiftOverflow) {
  struct Form {
    const char *Name;
    uint32_t Word;
    bool Position, Decrement, Wide, Variable;
  };
  constexpr Form Forms[] = {
#define NEVERD_TEST_DSP64_EXTRACT(Name, Word, Position, Decrement, Wide, Var)  \
  {Name, Word, Position, Decrement, Wide, Var},
#include "MIPSDSP64ExtractFixtures.def"
  };
  constexpr uint64_t Seeds[][2] = {{0, UINT64_MAX},
                                   {UINT64_MAX, 0},
                                   {0x123456789abcdef0, 0xfedcba9876543210},
                                   {0x8000000000000000, 0x8000000000000001}};
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    const auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    D.setText(true);
    for (const auto &F : Forms)
      for (unsigned Bank = 0; Bank < 4; ++Bank)
        for (unsigned Count = 0; Count < (F.Variable ? 64u : 32u); ++Count) {
          const auto Word =
              F.Word | (Bank << 11) | (F.Variable ? 0u : Count << 21);
          SCOPED_TRACE(::testing::Message()
                       << F.Name << " bank=" << Bank << " count=" << Count);
          const auto Code = encodeDSP({Word}, Target);
          DecodedInsn I;
          ASSERT_EQ(D.decodeOne(Code.data(), Code.size(), 0x10000, I), 4);
          EXPECT_STREQ(I.Raw->mnemonic, F.Name);
          EXPECT_TRUE(std::equal(Code.begin(), Code.end(), I.Raw->bytes));
          const auto Ops = liftDSP(D, Target, Word);
          for (unsigned Pos = 0; Pos < (F.Position ? 128u : 1u); ++Pos) {
            for (const auto &Seed : Seeds) {
              NdOpEmulator E(Image);
              E.setStrictMode(true);
              E.setRegister(mipsreg::A0, Count + 192);
              E.setRegister(mipsreg::AC0Pending, 1);
              const uint32_t Control = 0xa5204000 | Pos;
              E.setRegister(mipsreg::DSPControl, Control);
              for (unsigned B = 0; B < 4; ++B) {
                E.setRegister(mipsreg::accumulatorHI(B),
                              B == Bank ? Seed[0] : 11);
                E.setRegister(mipsreg::accumulatorLO(B),
                              B == Bank ? Seed[1] : 22);
              }
              for (const auto &Op : Ops)
                ASSERT_TRUE(E.step(Op)) << ndOpName(Op.Opcode);
              uint32_t ExpectedControl = Control;
              if (F.Position && Pos < Count) {
                EXPECT_FALSE(E.getRegister(mipsreg::V0));
                ExpectedControl |= 0x4000;
              } else {
                const auto Acc =
                    llvm::APInt(64, Seed[0]).concat(llvm::APInt(64, Seed[1]));
                auto Result = Acc.lshr(F.Position ? Pos - Count : Count);
                if (F.Position) {
                  Result &= llvm::APInt::getLowBitsSet(128, Count + 1);
                  ExpectedControl &= ~0x4000u;
                  if (F.Decrement)
                    ExpectedControl =
                        (ExpectedControl & ~127u) | ((Pos - Count - 1) & 127);
                }
                const auto Value =
                    F.Wide ? Result.trunc(64) : Result.trunc(32).sext(64);
                EXPECT_EQ(E.getRegister(mipsreg::V0), Value.getZExtValue());
              }
              EXPECT_EQ(E.getRegister(mipsreg::DSPControl), ExpectedControl);
              EXPECT_EQ(E.getRegister(mipsreg::AC0Pending), 1u);
              for (unsigned B = 0; B < 4; ++B) {
                EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(B)),
                          B == Bank ? Seed[0] : 11u);
                EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(B)),
                          B == Bank ? Seed[1] : 22u);
              }
            }
          }
        }
  }
}

TEST(MIPSDSP64, SignedShiftCountsAndMovePreserveFullAccumulatorState) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    const auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    D.setText(true);
    for (unsigned Bank = 0; Bank < 4; ++Bank)
      for (int Count = -64; Count < 64; ++Count)
        for (bool Variable : {false, true}) {
          const uint32_t Word = (Variable ? 0x7c8006fc : 0x7c0006bc) |
                                (Bank << 11) |
                                (Variable ? 0u : (unsigned(Count) & 127) << 19);
          const auto Code = encodeDSP({Word}, Target);
          DecodedInsn I;
          ASSERT_EQ(D.decodeOne(Code.data(), Code.size(), 0x10000, I), 4);
          EXPECT_STREQ(I.Raw->mnemonic, Variable ? "dshilov" : "dshilo");
          if (!Variable) {
            ASSERT_EQ(I.Raw->detail->mips.op_count, 2);
            EXPECT_EQ(I.Raw->detail->mips.operands[1].imm, Count);
            EXPECT_NE(std::string(I.Raw->op_str).find(std::to_string(Count)),
                      std::string::npos);
          }
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(mipsreg::A0, Count + 256);
          E.setRegister(mipsreg::DSPControl, 0xa5204021);
          E.setRegister(mipsreg::AC0Pending, 1);
          for (unsigned B = 0; B < 4; ++B) {
            E.setRegister(mipsreg::accumulatorHI(B),
                          B == Bank ? 0x123456789abcdef0 : 11);
            E.setRegister(mipsreg::accumulatorLO(B),
                          B == Bank ? 0xfedcba9876543210 : 22);
          }
          ASSERT_TRUE(executeDSP(D, Target, Word, E));
          const auto Acc = llvm::APInt(64, 0x123456789abcdef0)
                               .concat(llvm::APInt(64, 0xfedcba9876543210));
          const auto Expected = Count < 0 ? Acc.shl(-Count) : Acc.lshr(Count);
          for (unsigned B = 0; B < 4; ++B) {
            EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(B)),
                      B == Bank ? Expected.extractBitsAsZExtValue(64, 64)
                                : 11u);
            EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(B)),
                      B == Bank ? Expected.extractBitsAsZExtValue(64, 0) : 22u);
          }
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0xa5204021u);
          EXPECT_EQ(E.getRegister(mipsreg::AC0Pending), unsigned(Bank != 0));
        }
    for (unsigned Bank = 0; Bank < 4; ++Bank)
      for (unsigned Pos = 0; Pos < 128; ++Pos) {
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, 0x89abcdef01234567);
        E.setRegister(mipsreg::accumulatorHI(Bank), 11);
        E.setRegister(mipsreg::accumulatorLO(Bank), 0xfedcba9876543210);
        E.setRegister(mipsreg::DSPControl, 0xa5204000 | Pos);
        E.setRegister(mipsreg::AC0Pending, 1);
        ASSERT_TRUE(executeDSP(D, Target, 0x7c8007fc | (Bank << 11), E));
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(Bank)),
                  0xfedcba9876543210u);
        EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank)),
                  0x89abcdef01234567u);
        if (Pos <= 64)
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                    0xa5204000u | ((Pos + 64) & 127));
        else
          EXPECT_FALSE(E.getRegister(mipsreg::DSPControl));
        ASSERT_TRUE(
            executeDSP(D, Target, 0x7c3e14b8, E)); // RDDSP excludes POS.
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0xa5204000u);
        EXPECT_EQ(E.getRegister(mipsreg::AC0Pending), unsigned(Bank != 0));
      }
  }
}

TEST(MIPSDSP64, ExtractionAdmissionRejectsReservedFieldsAndWrongWidths) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    for (uint32_t Word : {0x7c8200fc, 0x7c82047c, 0x7c0006bc, 0x7c8006fc,
                          0x7c8007fc, 0x7c82000d}) {
      const auto Code = encodeDSP({Word}, Target);
      DecodedInsn I;
      ASSERT_EQ(D.decodeOne(Code.data(), Code.size(), 0x10000, I), 4);
      for (unsigned Bit : {13u, 14u, 15u}) {
        cs_insn Bad = *I.Raw;
        const auto Invalid = encodeDSP({Word | (1u << Bit)}, Target);
        std::copy(Invalid.begin(), Invalid.end(), Bad.bytes);
        MIPSLifter L(Target);
        L.setDSPEnabled(true);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(Bad, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
      for (Arch Profile :
           {Target, archLittleEndian(Target) ? Arch::MIPS32EL : Arch::MIPS32}) {
        MIPSLifter L(Profile);
        L.setDSPEnabled(Profile != Target);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(*I.Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
    // EFI still changes when the GPR destination is zero, even with unknown AC.
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::DSPControl, 0);
    E.setRegister(mipsreg::A0, 63);
    ASSERT_TRUE(executeDSP(D, Target, 0x7c8002fc, E));
    EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x4000u);
  }
  mips::ScalarShape Shape{Arch::MIPS64, 3, true, true, 8, 1, 8, 0, true};
  EXPECT_TRUE(mips::scalarShapeIsValid(Intrinsic::MIPSExtractResult, Shape));
  Shape.Target = Arch::MIPS32;
  EXPECT_FALSE(mips::scalarShapeIsValid(Intrinsic::MIPSExtractResult, Shape));
  Shape.OutputBytes = Shape.SecondBytes = 4;
  EXPECT_TRUE(mips::scalarShapeIsValid(Intrinsic::MIPSExtractResult, Shape));
  Shape.SecondBytes = 8;
  EXPECT_FALSE(mips::scalarShapeIsValid(Intrinsic::MIPSExtractResult, Shape));
}

TEST(MIPSDSP64, InsertValidatesFullPositionAndPreservesOtherBits) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    const auto Image = dspImage(Target);
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    for (unsigned Pos = 0; Pos < 128; ++Pos)
      for (unsigned Size = 0; Size < 64; ++Size)
        for (bool Alias : {false, true}) {
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          constexpr uint64_t Source = 0x0123456789abcdef;
          constexpr uint64_t Destination = 0xfedcba9876543210;
          const auto Control = 0xa5204000u | Pos | (Size << 7);
          E.setRegister(mipsreg::DSPControl, Control);
          E.setRegister(mipsreg::A0, Source);
          E.setRegister(mipsreg::V0, Destination);
          ASSERT_TRUE(
              executeDSP(D, Target, Alias ? 0x7c84000d : 0x7c82000d, E));
          const auto Result = E.getRegister(Alias ? mipsreg::A0 : mipsreg::V0);
          if (!Size || Pos + Size > 64)
            EXPECT_FALSE(Result);
          else {
            const uint64_t Mask = ((uint64_t(1) << Size) - 1) << Pos;
            EXPECT_EQ(Result, ((Alias ? Source : Destination) & ~Mask) |
                                  ((Source << Pos) & Mask));
          }
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl), Control);
        }
  }
}

INSTANTIATE_TEST_SUITE_P(BothWidthsAndByteOrders, MIPSDSPRegisterWidth,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

TEST_P(MIPSDSPRegisterWidth, IndexedDoublewordUsesTheEntireEffectiveAddress) {
  auto Image = dspImage(GetParam());
  const bool Wide = isMIPS64(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  const auto Bytes = encodeDSP({0x7c85120a}, GetParam()); // LDX v0,a1(a0)
  DecodedInsn I{};
  if (!Wide) {
    if (D.decodeOne(Bytes.data(), 4, 0x10000, I)) {
      std::vector<LowOp> Ops;
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
    }
    return;
  }
  ASSERT_EQ(D.decodeOne(Bytes.data(), 4, 0x10000, I), 4);
  EXPECT_STREQ(I.Raw->mnemonic, "ldx");
  EXPECT_EQ(I.Id, MIPSLifter::IndexedLoadDoublewordID);
  EXPECT_TRUE(std::equal(Bytes.begin(), Bytes.end(), I.Raw->bytes));
  std::vector<LowOp> Ops;
  D.liftToLow(I, Ops);
  const auto Address = llvm::find_if(
      Ops, [](const auto &O) { return O.Opcode == NdOp::INT_ADD; });
  ASSERT_NE(Address, Ops.end());
  EXPECT_EQ(Address->Inputs[0].Size, 8);
  EXPECT_EQ(Address->Inputs[1].Size, 8);
  EXPECT_EQ(Address->Output.Size, 8);
  const auto Load =
      llvm::find_if(Ops, [](const auto &O) { return O.Opcode == NdOp::LOAD; });
  ASSERT_NE(Load, Ops.end());
  EXPECT_EQ(Load->Output.Size, 8);
  Segment Data;
  Data.VA = 0x100002000ull;
  Data.Data = {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::A0, 0x100001ff8ull);
  E.setRegister(mipsreg::A1, 8);
  ASSERT_TRUE(llvm::all_of(Ops, [&](const auto &O) { return E.step(O); }));
  const auto Expected = archLittleEndian(GetParam()) ? 0xefcdab8967452381ull
                                                     : 0x8123456789abcdefull;
  EXPECT_EQ(E.getRegister(mipsreg::V0), Expected);
  E.setRegister(mipsreg::A1, 9);
  EXPECT_FALSE(llvm::all_of(Ops, [&](const auto &O) { return E.step(O); }));
  EXPECT_EQ(E.getRegister(mipsreg::V0), Expected);
}

TEST_P(MIPSDSPRegisterWidth, BankMovesAndWordAccumulationHaveDifferentWidths) {
  const auto Image = dspImage(GetParam());
  const bool Wide = isMIPS64(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  const uint64_t High = Wide ? 0x1234567887654321ull : 0x87654321u;
  const uint64_t Low = Wide ? 0xfedcba9876543210ull : 0x76543210u;
  E.setRegister(mipsreg::A0, High);
  E.setRegister(mipsreg::A1, Low);
  // MD00375 rev 3.02 specifies full-width bank transfers; word arithmetic
  // instead sign-extends each half of the accumulator independently.
  for (auto Word : {0x00801011u, 0x00a01013u, 0x00401010u, 0x00401812u})
    ASSERT_TRUE(executeDSP(D, GetParam(), Word, E));
  EXPECT_EQ(E.getRegister(mipsreg::V0), High);
  EXPECT_EQ(E.getRegister(mipsreg::V1), Low);
  ASSERT_TRUE(executeDSP(D, GetParam(), 0x70001000, E)); // MADD ac2,zero,zero
  EXPECT_EQ(E.getRegister(mipsreg::AC2HI), Wide ? 0xffffffff87654321ull : High);
  EXPECT_EQ(E.getRegister(mipsreg::AC2LO), 0x76543210u);
}

TEST_P(MIPSDSPRegisterWidth, InsertValidatesBothWordInputsOnN64) {
  auto Image = dspImage(GetParam());
  const bool Wide = isMIPS64(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  for (const auto &Bad : {mipsreg::Zero, mipsreg::A0, mipsreg::A1}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0,
                  Wide && Bad == mipsreg::A0 ? 0x8000000000000007ull : 7);
    E.setRegister(mipsreg::A1,
                  Wide && Bad == mipsreg::A1 ? 0x8000000000000050ull : 0x50);
    E.setRegister(mipsreg::DSPControl, 4u << 7);
    ASSERT_TRUE(executeDSP(D, GetParam(), 0x7c85000c, E));
    if (Wide && Bad != mipsreg::Zero)
      EXPECT_FALSE(E.getRegister(mipsreg::A1));
    else
      EXPECT_EQ(E.getRegister(mipsreg::A1), 0x57u);
    EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 4u << 7);
  }
}

TEST_P(MIPSDSPRegisterWidth, RevisionTwoMatchesEveryIndependentMachineRecord) {
  struct Record {
    const char *Mnemonic;
    uint32_t Word, V0, Control, HI2, LO2;
  };
  static constexpr Record Cases[] = {
#define MIPS_DSPR2_CASE(Name, Word, V0, Control, HI2, LO2)                     \
  {#Name, Word, V0, Control, HI2, LO2},
#include "MIPSDSPR2Fixtures.def"
  };
  static_assert(std::size(Cases) == 46);
  auto Image = dspImage(GetParam());
  const bool Wide = isMIPS64(GetParam());
  const auto WordValue = [Wide](uint32_t V) -> uint64_t {
    return Wide ? uint64_t(int64_t(int32_t(V))) : V;
  };
  Image.MIPS->HasDSPR2 = true;
  Decoder D;
  ASSERT_TRUE(D.init(Image));
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
  uc_engine *Raw = nullptr;
  const auto Mode =
      static_cast<uc_mode>((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                           (archLittleEndian(GetParam()) ? UC_MODE_LITTLE_ENDIAN
                                                         : UC_MODE_BIG_ENDIAN));
  ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, Wide ? int(UC_CPU_MIPS64_MIPS64DSPR2)
                                           : int(UC_CPU_MIPS32_74KF)),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
  uint64_t PC = 0x10000;
  const auto RunReference = [&](llvm::ArrayRef<uint32_t> Words) {
    const auto Code = encodeDSP(Words, GetParam());
    ASSERT_EQ(uc_mem_write(Raw, PC, Code.data(), Code.size()), UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(Raw, PC, PC + Code.size(), 0, Words.size()),
              UC_ERR_OK);
    PC += Code.size();
  };
  RunReference(
      {0x40086000, 0x3c090100, 0x01094025, 0x40886000, 0xc0}); // Status.MX
#endif
  for (const auto &R : Cases) {
    SCOPED_TRACE(R.Mnemonic);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A1, WordValue(0x80007fff));
    E.setRegister(mipsreg::A2, WordValue(0x80ff8000));
    E.setRegister(mipsreg::A3, WordValue(0x8000ffff));
    E.setRegister(mipsreg::V0, WordValue(0x80ff8000));
    E.setRegister(mipsreg::DSPControl, 0x01004005);
    E.setRegister(mipsreg::AC2HI, WordValue(0x80007fff));
    E.setRegister(mipsreg::AC2LO, WordValue(0x80ff8000));
    ASSERT_TRUE(executeDSP(D, GetParam(), R.Word, E));
    EXPECT_EQ(E.getRegister(mipsreg::V0), WordValue(R.V0));
    EXPECT_EQ(E.getRegister(mipsreg::DSPControl), R.Control);
    EXPECT_EQ(E.getRegister(mipsreg::AC2HI), WordValue(R.HI2));
    EXPECT_EQ(E.getRegister(mipsreg::AC2LO), WordValue(R.LO2));
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
    const auto Set = [&](int Reg, uint32_t Value) {
      const auto Full = WordValue(Value);
      ASSERT_EQ(uc_reg_write(Raw, Reg,
                             Wide ? static_cast<const void *>(&Full)
                                  : static_cast<const void *>(&Value)),
                UC_ERR_OK);
    };
    Set(UC_MIPS_REG_4, 0x01004005);
    Set(UC_MIPS_REG_5, 0x80007fff);
    Set(UC_MIPS_REG_6, 0x80ff8000);
    Set(UC_MIPS_REG_7, 0x8000ffff);
    Set(UC_MIPS_REG_2, 0x80ff8000);
    // WRDSP, MTHI/MTLO ac2, instruction under test, RDDSP, MFHI/MFLO ac2.
    RunReference({0x7c81fcf8, 0x00a01011, 0x00c01013, R.Word, 0x7c3f44b8,
                  0x00404810, 0x00405012});
    for (const auto &[Reg, Want] :
         std::array{std::pair{UC_MIPS_REG_2, mipsreg::V0},
                    std::pair{UC_MIPS_REG_8, mipsreg::DSPControl},
                    std::pair{UC_MIPS_REG_9, mipsreg::AC2HI},
                    std::pair{UC_MIPS_REG_10, mipsreg::AC2LO}}) {
      uint64_t Full = 0;
      uint32_t Low = 0;
      ASSERT_EQ(uc_reg_read(Raw, Reg,
                            Wide ? static_cast<void *>(&Full)
                                 : static_cast<void *>(&Low)),
                UC_ERR_OK);
      EXPECT_EQ(E.getRegister(Want), Wide ? Full : Low);
    }
#endif
  }
}
TEST_P(MIPSDSP, RevisionTwoRequiresEvidenceAndPreservesLaneEffects) {
  auto Image = dspImage(GetParam());
  Decoder D;
  const auto Word = [](unsigned Fields, unsigned Destination) {
    return 0x7c000000u | (4u << 21) | (5u << 16) | (Destination << 11) | Fields;
  };
  ASSERT_TRUE(D.init(Image));
  EXPECT_THROW(liftDSP(D, GetParam(), Word(0x18, 2)), UnliftedInstruction);
  Image.MIPS->HasDSPR2 = true;
  ASSERT_TRUE(D.init(Image));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::DSPControl, 0x04004012);
  E.setRegister(mipsreg::A0, 0x8000ffff);
  E.setRegister(mipsreg::A1, 0xffff0001);
  ASSERT_TRUE(executeDSP(D, GetParam(), Word(0x18, 2), E));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0xbf7f7f80u);
  ASSERT_TRUE(executeDSP(D, GetParam(), Word(0x310, 0), E));
  EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x04104012u);
  ASSERT_TRUE(executeDSP(D, GetParam(), Word(0x651, 2), E));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 12u);
  EXPECT_EQ(E.getRegister(mipsreg::DSPControl), 0x0c104012u);
  // Two physical inputs, an aliased destination and no carry across lanes.
  ASSERT_TRUE(executeDSP(D, GetParam(), Word(0x218, 4), E));
  EXPECT_EQ(E.getRegister(mipsreg::A0), 0xbfff0000u);
  Image.MIPS->HasDSPR2 = false;
  ASSERT_TRUE(D.init(Image));
  EXPECT_THROW(liftDSP(D, GetParam(), Word(0x310, 2)), UnliftedInstruction);
}

TEST_P(MIPSDSP, PartialControlTracksOnlyArchitecturallyUndefinedFields) {
  auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::DSPControl, 0x02104021);
  E.setRegister(mipsreg::A0, 0x8000ffff);
  E.setRegister(mipsreg::A1, 0x7fffffff);
  ASSERT_TRUE(executeDSP(D, GetParam(), 0x7c850251, E)); // cmp.lt.ph
  EXPECT_FALSE(E.getRegister(mipsreg::DSPControl));
  ASSERT_TRUE(executeDSP(D, GetParam(), 0x7c0114b8, E)); // rddsp v0,1 (POS)
  EXPECT_EQ(E.getRegister(mipsreg::V0), 33u);
  E.setRegister(mipsreg::AC2LO, 0x13572468);
  ASSERT_TRUE(
      executeDSP(D, GetParam(), 0x7c8017f8, E)); // mthlip a0,ac2: POS > 32
  EXPECT_EQ(E.getRegister(mipsreg::AC2HI), 0x13572468u);
  EXPECT_EQ(E.getRegister(mipsreg::AC2LO), 0x8000ffffu);
  ASSERT_TRUE(executeDSP(D, GetParam(), 0x7c0814b8, E)); // rddsp v0,8 (OUflag)
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0x00100000u);
}

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

TEST_P(MIPSDSP, ExpandedMultiplyAndPrecisionReductionPreserveExactStickyState) {
  auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  const uint32_t Encodings[] = {0x7c000710, 0x7c000750, 0x7c000551, 0x7c0003d1};
  const uint32_t Inputs[] = {0,          1,          0xffffffff, 0x80008000,
                             0x7fff7fff, 0x7fff8000, 0xffff7fff, 0xffff8000,
                             0x7f807f80, 0x7f817f81, 0x007f0080, 0x80000000,
                             0x7fffffff};
  for (unsigned Op = 0; Op != 4; ++Op)
    for (auto A : Inputs)
      for (auto B : Inputs)
        for (unsigned RD : {0u, 2u, 4u, 5u}) {
          uint32_t Value = 0;
          bool Overflow = false;
          const auto Signed = [](uint32_t V, unsigned Bits) -> int64_t {
            return V & (uint32_t(1) << (Bits - 1))
                       ? int64_t(V) - (INT64_C(1) << Bits)
                       : int64_t(V);
          };
          if (Op < 2) {
            unsigned Shift = Op ? 0 : 16;
            int64_t X = Signed((A >> Shift) & 65535, 16),
                    Y = Signed((B >> Shift) & 65535, 16);
            Overflow = X == -32768 && Y == -32768;
            Value = Overflow ? 0x7fffffff : uint32_t(X * Y * 2);
          } else if (Op == 2) {
            for (auto V : {A, B}) {
              int64_t Rounded = Signed(V, 32) + 32768;
              Overflow |= Rounded > INT32_MAX;
              uint32_t Lane = Rounded > INT32_MAX
                                  ? 32767
                                  : uint32_t(uint64_t(Rounded) >> 16) & 65535;
              Value = (Value << 16) | Lane;
            }
          } else {
            for (auto V : {A, B})
              for (unsigned Shift : {16u, 0u}) {
                int64_t X = Signed((V >> Shift) & 65535, 16);
                Overflow |= X < 0 || X > 32640;
                Value = (Value << 8) | (X < 0       ? 0
                                        : X > 32640 ? 255
                                                    : uint32_t(X / 128));
              }
          }
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          uint32_t Control = RD & 1 ? 0x0fff7fbf : 0x01000021;
          E.setRegister(mipsreg::DSPControl, Control);
          E.setRegister(mipsreg::A0, A);
          E.setRegister(mipsreg::A1, B);
          E.setRegister(mipsreg::HI, 123);
          E.setRegister(mipsreg::LO, 456);
          ASSERT_TRUE(executeDSP(
              D, GetParam(), Encodings[Op] | (4 << 21) | (5 << 16) | (RD << 11),
              E));
          if (RD)
            EXPECT_EQ(E.getRegister(mipsreg::r(RD)), Value);
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                    Control | (Overflow ? 1u << (Op < 2 ? 21 : 22) : 0));
          if (Op >= 2) {
            EXPECT_EQ(E.getRegister(mipsreg::HI), 123u);
            EXPECT_EQ(E.getRegister(mipsreg::LO), 456u);
          } else {
            LowOp Observe;
            Observe.Opcode = NdOp::INT_ADD;
            Observe.Output = NdVar::tmp(9000, 4);
            Observe.addInput(NdVar::reg(mipsreg::HI, 4));
            Observe.addInput(NdVar::cst(0, 4));
            EXPECT_FALSE(E.step(Observe));
            Observe.Inputs[0] = NdVar::reg(mipsreg::LO, 4);
            EXPECT_FALSE(E.step(Observe));
          }
        }
  Decoder Plain;
  ASSERT_TRUE(Plain.init(GetParam()));
  for (auto Word : Encodings)
    EXPECT_THROW(
        liftDSP(Plain, GetParam(), Word | (4 << 21) | (5 << 16) | (2 << 11)),
        UnliftedInstruction);
}

TEST_P(MIPSDSP, AccumulatorBanksAndDotProductsPreserveFullCarryAndStickyFlags) {
  auto I = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  const uint32_t Ops[] = {0x7c8500f0, 0x7c8501f0, 0x7c8502f0, 0x7c8503f0,
                          0x7c850130, 0x7c850170, 0x7c8501b0};
  for (unsigned Bank = 0; Bank != 4; ++Bank)
    for (unsigned Kind = 0; Kind != 7; ++Kind)
      for (auto [A, B] : {std::pair{0xffffffffu, 0xffffffffu},
                          std::pair{0x80008000u, 0x80008000u},
                          std::pair{0x80007fffu, 0x7fff8000u},
                          std::pair{0x12345678u, 0x90abcdefu}})
        for (uint64_t Initial :
             {UINT64_C(0), UINT64_MAX, UINT64_C(0x80000000ffffffff)}) {
          NdOpEmulator E(I);
          E.setStrictMode(true);
          for (unsigned N = 0; N != 4; ++N) {
            E.setRegister(mipsreg::accumulatorHI(N),
                          N == Bank ? Initial >> 32 : 0xabcdef01u);
            E.setRegister(mipsreg::accumulatorLO(N),
                          N == Bank ? uint32_t(Initial) : 0x12345678u);
          }
          E.setRegister(mipsreg::A0, A);
          E.setRegister(mipsreg::A1, B);
          E.setRegister(mipsreg::DSPControl, 0x05a17fbf);
          uint64_t Sum = 0;
          uint32_t Control = 0x05a17fbf;
          for (unsigned N = 0; N != 2; ++N) {
            int64_t Product;
            if (Kind < 4) {
              const unsigned Shift = 8 * (N + (!(Kind & 1) ? 2 : 0));
              Product = ((A >> Shift) & 255) * ((B >> Shift) & 255);
            } else {
              const unsigned Shift = 16 * (1 - N);
              const int64_t X = int((A >> Shift & 65535) ^ 32768) - 32768;
              const int64_t Y = int((B >> Shift & 65535) ^ 32768) - 32768;
              if (X == -32768 && Y == -32768) {
                Product = 0x7fffffff;
                Control |= 1u << (16 + Bank);
              } else
                Product = 2 * X * Y;
            }
            Sum += uint64_t(Kind == 6 && N ? -Product : Product);
          }
          const uint64_t Expected = Kind == 2 || Kind == 3 || Kind == 5
                                        ? Initial - Sum
                                        : Initial + Sum;
          ASSERT_TRUE(executeDSP(D, GetParam(), Ops[Kind] | (Bank << 11), E));
          EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(Bank)),
                    Expected >> 32);
          EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank)),
                    uint32_t(Expected));
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl), Control);
          for (unsigned N = 0; N != 4; ++N)
            if (N != Bank) {
              EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(N)), 0xabcdef01u);
              EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(N)), 0x12345678u);
            }
        }
  for (unsigned Bank = 1; Bank != 4; ++Bank) {
    NdOpEmulator E(I);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 0x87654321);
    E.setRegister(mipsreg::HI, 19);
    E.setRegister(mipsreg::LO, 23);
    for (bool High : {false, true}) {
      ASSERT_TRUE(executeDSP(D, GetParam(),
                             0x00800011 | (Bank << 11) | (High ? 0 : 2), E));
      ASSERT_TRUE(executeDSP(D, GetParam(),
                             0x00001010 | (Bank << 21) | (High ? 0 : 2), E));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0x87654321u);
    }
    EXPECT_EQ(E.getRegister(mipsreg::HI), 19u);
    EXPECT_EQ(E.getRegister(mipsreg::LO), 23u);
  }
}

TEST_P(MIPSDSP,
       ExtractsUseBothOverflowTestsAllCountsAndConditionallyAdvancePosition) {
  const auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  // Independent llvm-mc literals with $v0 destination and immediate 0 / $a0.
  const uint32_t Words[] = {0x7c020038, 0x7c020138, 0x7c0201b8,
                            0x7c0203b8, 0x7c0200b8, 0x7c0202b8};
  for (unsigned Bank : {0u, 3u})
    for (unsigned Count = 0; Count < 32; ++Count)
      for (int64_t Acc :
           {INT64_MIN, INT64_MAX, INT64_C(-4294967297), INT64_C(4294967295),
            INT64_C(-32769), INT64_C(32768), INT64_C(-1)})
        for (bool Variable : {false, true})
          for (unsigned K = 0; K < 6; ++K) {
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            E.setRegister(mipsreg::accumulatorHI(Bank), uint64_t(Acc) >> 32);
            E.setRegister(mipsreg::accumulatorLO(Bank), uint32_t(Acc));
            E.setRegister(mipsreg::A0, Count + 64);
            const unsigned Pos = (Count & 1) ? Count - 1 : Count;
            uint32_t Control = 0x05244000 | Pos;
            E.setRegister(mipsreg::DSPControl, Control);
            const auto W = Words[K] | (Bank << 11) |
                           (Variable ? ((4u << 21) | 64u) : Count << 21);
            ASSERT_TRUE(executeDSP(D, GetParam(), W, E)) << std::hex << W;
            if (K >= 4) {
              if (Pos < Count)
                EXPECT_FALSE(E.getRegister(mipsreg::V0));
              else {
                EXPECT_EQ(E.getRegister(mipsreg::V0),
                          (uint64_t(Acc) >> (Pos - Count)) &
                              (UINT32_MAX >> (31 - Count)));
                Control &= ~0x4000u;
                if (K == 5)
                  Control = (Control & ~63u) | ((Pos - Count - 1) & 63);
              }
            } else {
              const auto Floor = Acc >> Count;
              const auto Nearest =
                  Floor + (Count ? (uint64_t(Acc) >> (Count - 1)) & 1u : 0u);
              const int64_t Lower = K == 3 ? INT16_MIN : INT32_MIN,
                            Upper = K == 3 ? INT16_MAX : INT32_MAX;
              if (Floor < Lower || (K == 3 ? Floor : int64_t(Nearest)) > Upper)
                Control |= 1u << 23;
              const auto V = K == 0   ? Floor
                             : K == 3 ? std::clamp(Floor, Lower, Upper)
                             : K == 2
                                 ? std::clamp(int64_t(Nearest), Lower, Upper)
                                 : int64_t(Nearest);
              EXPECT_EQ(E.getRegister(mipsreg::V0), uint32_t(V));
            }
            EXPECT_EQ(E.getRegister(mipsreg::DSPControl), Control);
            EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank)),
                      uint32_t(Acc));
          }
}

TEST_P(MIPSDSP, FractionalAccumulatorSaturationChecksProductAndFinalSum) {
  const auto Image = dspImage(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  constexpr uint32_t Words[] = {0x7c850330, 0x7c850370, 0x7c850430,
                                0x7c8504b0, 0x7c850530, 0x7c8505b0};
  for (unsigned K = 0; K < 6; ++K)
    for (unsigned Bank = 0; Bank < 4; ++Bank)
      for (int64_t Initial :
           {INT64_MIN, INT64_MAX, INT64_C(-2147483649), INT64_C(1)})
        for (auto [A, B] : {std::pair{0x80000000u, 0x80000000u},
                            std::pair{0x80018000u, 0x7fff8000u},
                            std::pair{0x7fffffffu, 1u}}) {
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(mipsreg::accumulatorHI(Bank), uint64_t(Initial) >> 32);
          E.setRegister(mipsreg::accumulatorLO(Bank), uint32_t(Initial));
          E.setRegister(mipsreg::DSPControl, 0x05404021);
          E.setRegister(mipsreg::A0, A);
          E.setRegister(mipsreg::A1, B);
          ASSERT_TRUE(executeDSP(D, GetParam(), Words[K] | (Bank << 11), E));
          const auto Part = [&](uint32_t X) -> int64_t {
            return K < 2 ? int32_t(X) : int16_t(X >> (K % 2 ? 0 : 16));
          };
          llvm::APInt P =
              llvm::APInt(128, Part(A), true) * llvm::APInt(128, Part(B), true);
          P <<= 1;
          const unsigned Width = K < 2 ? 64 : 32;
          const auto Max = llvm::APInt::getSignedMaxValue(Width).sext(128);
          const auto Min = llvm::APInt::getSignedMinValue(Width).sext(128);
          bool Sat = P.sgt(Max);
          if (Sat)
            P = Max;
          auto V = llvm::APInt(128, Initial, true) + (K == 1 ? -P : P);
          if (K >= 2)
            V = V.trunc(64).sext(128);
          if (K < 4 && (V.sgt(Max) || V.slt(Min))) {
            Sat = true;
            V = V.sgt(Max) ? Max : Min;
          }
          const auto R = V.trunc(64).getZExtValue();
          EXPECT_EQ(E.getRegister(mipsreg::accumulatorHI(Bank)), R >> 32);
          EXPECT_EQ(E.getRegister(mipsreg::accumulatorLO(Bank)), uint32_t(R));
          EXPECT_EQ(E.getRegister(mipsreg::DSPControl),
                    0x05404021u | (unsigned(Sat) << (16 + Bank)));
        }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSDSP,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace

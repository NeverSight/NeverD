//===- MIPSHistoricalTests.cpp - MIPS I-V instruction boundaries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal words distinguish architecture levels, modern releases and FCC
/// views.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSHistoricalISA.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalarFPOperations.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "neverd/emulation/arch/mips/ScalarFP.h"
#endif

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd {
namespace {
cs_insn instruction(uint32_t Word, Arch Target) {
  cs_insn Insn{};
  Insn.address = 0x10000;
  Insn.size = 4;
  llvm::support::endian::write32(Insn.bytes, Word,
                                 archLittleEndian(Target)
                                     ? llvm::endianness::little
                                     : llvm::endianness::big);
  return Insn;
}
BinaryImage historicalImage(Arch Target, unsigned Level) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Bitness::Bits64;
  Image.MIPS.emplace();
  Image.MIPS->ABI = isMIPSN32(Target) ? MIPSABI::N32 : MIPSABI::N64;
  Image.MIPS->ISARevision = 0;
  Image.MIPS->HistoricalISA = Level;
  Image.MIPS->FPABI = 1;
  return Image;
}
TEST(MIPSHistorical, AvailabilityUsesPhysicalISAInsteadOfReleaseNumbers) {
  struct Case {
    uint32_t Word;
    unsigned First;
  };
  constexpr Case Cases[] = {{0x00851021, 1},  // ADDU
                            {0x0085100a, 4},  // MOVZ
                            {0x0085102d, 3},  // DADDU
                            {0x50850001, 2},  // BEQL
                            {0x46002084, 2},  // SQRT.S
                            {0x46002088, 3},  // ROUND.L.S
                            {0x46002095, 4},  // RECIP.S
                            {0x46c62080, 5},  // ADD.PS
                            {0x460620a6, 5},  // CVT.PS.S
                            {0x4c862020, 4},  // MADD.S
                            {0x4c862026, 5},  // MADD.PS
                            {0x4c850005, 5},  // LUXC1
                            {0x45090001, 4},  // BC1T FCC2
                            {0x46062232, 4},  // C.EQ.S FCC2
                            {0x00251082, 0},  // ROTR
                            {0x7c851000, 0},  // EXT
                            {0x70851002, 0},  // MUL
                            {0x4442c800, 5},  // CFC1 FCCR
                            {0x44622000, 0},  // MFHC1
                            {0x46062098, 0},  // R6 MADDF
                            {0x00201010, 0},  // DSP MFHI bank 1
                            {0x00850818, 0},  // DSP MULT bank 1
                            {0x78000000, 0}}; // MSA
  for (const auto &C : Cases)
    for (unsigned Level = 1; Level <= 5; ++Level)
      EXPECT_EQ(mips::historicalInstructionAvailable(C.Word, Level),
                C.First != 0 && Level >= C.First)
          << std::hex << C.Word << " level=" << Level;
}
TEST(MIPSHistorical, DecoderReuseAndDirectLiftEnforceTheSameProfile) {
  for (Arch Target :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    Decoder D;
    for (unsigned Level : {5, 4, 5}) {
      auto Image = historicalImage(Target, Level);
      ASSERT_TRUE(D.init(Image));
      MIPSLifter L(Target, InstructionMode::Default, 0, Level);
      L.setScalarFPEnabled(true);
      for (uint32_t Word :
           {0x0085102du, 0x46c62080u, 0x7c851000u, 0x4442c800u, 0x44622000u}) {
        auto I = instruction(Word, Target);
        DecodedInsn Decoded;
        const bool Expected =
            Word == 0x0085102d ||
            ((Word == 0x46c62080 || Word == 0x4442c800) && Level == 5);
        EXPECT_EQ(D.decodeOne(I.bytes, 4, I.address, Decoded),
                  Expected ? 4 : 0);
        std::vector<LowOp> Ops;
        if (Expected) {
          EXPECT_NO_THROW(L.lift(I, Ops));
          EXPECT_FALSE(Ops.empty());
        } else {
          EXPECT_THROW(L.lift(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
      // These bit patterns were ordinary SLL-to-zero instructions before R2.
      for (uint32_t Word : {0xc0u, 0x140u}) {
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(L.lift(instruction(Word, Target), Ops));
        for (const auto &Op : Ops)
          EXPECT_NE(Op.Opcode, NdOp::INTRINSIC);
        auto Slot = instruction(Word, Target);
        Slot.address += 4;
        Ops.clear();
        EXPECT_NO_THROW(L.liftUnit(instruction(0x03e00008, Target), Slot, Ops));
      }
    }
  }
}
TEST(MIPSHistorical, TimingAndCompressedProfilesCannotBypassAdmission) {
  const auto I = instruction(0x00851021, Arch::MIPS64);
  for (unsigned Level : {1u, 2u, 3u, 6u}) {
    auto Image = historicalImage(Arch::MIPS64, Level);
    Decoder D;
    EXPECT_FALSE(D.init(Image));
    MIPSLifter L(Arch::MIPS64, InstructionMode::Default, 0, Level);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(I, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
  for (InstructionMode Mode :
       {InstructionMode::MicroMIPS, InstructionMode::MIPS16}) {
    MIPSLifter L(Arch::MIPS64, Mode, 0, 5);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(I, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
  for (unsigned Level : {4u, 5u})
    for (uint8_t Revision : {2, 5, 6}) {
      auto Image = historicalImage(Arch::MIPS64, Level);
      Image.MIPS->ISARevision = Revision;
      Decoder D;
      EXPECT_FALSE(D.init(Image));
      MIPSLifter L(Arch::MIPS64, InstructionMode::Default, Revision, Level);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(I, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(MIPSHistorical, PairedRearrangementAliasesKeepPhysicalBytesAndOperands) {
  struct Case {
    uint32_t Word;
    unsigned ID;
    const char *Name;
    unsigned Operands;
  };
  constexpr Case Cases[] = {
      {0x460620a6, MIPS_INS_CVT_PS_S, "cvt.ps.s", 3},
      {0x46c020a0, MIPS_INS_CVT_S_PU, "cvt.s.pu", 2},
      {0x46c020a8, MIPS_INS_CVT_S_PL, "cvt.s.pl", 2},
      {0x46c620ac, MIPS_INS_PLL_PS, "pll.ps", 3},
      {0x46c620ad, MIPS_INS_PLU_PS, "plu.ps", 3},
      {0x46c620ae, MIPS_INS_PUL_PS, "pul.ps", 3},
      {0x46c620af, MIPS_INS_PUU_PS, "puu.ps", 3},
      {0x4c86209e, MIPSLifter::AlignPairedSingleID, "alnv.ps", 4}};
  for (Arch Target :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    Decoder D;
    for (unsigned Level : {5, 4, 5}) {
      ASSERT_TRUE(D.init(historicalImage(Target, Level)));
      for (const auto &C : Cases) {
        const auto I = instruction(C.Word, Target);
        DecodedInsn Decoded;
        ASSERT_EQ(D.decodeOne(I.bytes, 4, I.address, Decoded),
                  Level == 5 ? 4 : 0);
        if (Level == 4)
          continue;
        EXPECT_EQ(Decoded.Raw->id, C.ID);
        EXPECT_STREQ(Decoded.Raw->mnemonic, C.Name);
        EXPECT_EQ(std::memcmp(Decoded.Raw->bytes, I.bytes, 4), 0);
        const auto &M = Decoded.Raw->detail->mips;
        ASSERT_EQ(M.op_count, C.Operands);
        EXPECT_EQ(M.operands[0].reg, MIPS_REG_F2);
        EXPECT_EQ(M.operands[1].reg, MIPS_REG_F4);
        if (C.Operands >= 3)
          EXPECT_EQ(M.operands[2].reg, MIPS_REG_F6);
        if (C.Operands == 4)
          EXPECT_EQ(M.operands[3].reg, MIPS_REG_A0);
      }
    }
  }
}
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
TEST(MIPSHistorical, ForgedModernFPAndAbsentControlViewsHaveNoEffects) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = historicalImage(Target, 4);
    auto FPU = emulation::createScalarFPExecutor(Target, 2);
    ASSERT_TRUE(bool(FPU));
    for (auto Id : {Intrinsic::MIPSFPADD_PS, Intrinsic::MIPSFPMADDF_S,
                    Intrinsic::MIPSReadFPControl}) {
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 0x12345678);
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.addInput(NdVar::cst(unsigned(Id), 4));
      if (Id == Intrinsic::MIPSReadFPControl) {
        Op.Output = NdVar::reg(mipsreg::V0, 4);
        Op.addInput(NdVar::cst(25, 1));
      } else {
        const auto &F = mips::scalarFPOperation(Id);
        Op.Output = NdVar::reg(mipsreg::V0, F.OutputBytes);
        for (unsigned I = 0; I < F.Arity; ++I)
          Op.addInput(NdVar::cst(0, F.InputBytes));
      }
      MIPSSystemContext C;
      C.ScalarFPU = FPU->get();
      C.FCSR = 0x800001;
      EXPECT_FALSE(E.stepMIPSSystem(Op, C));
      EXPECT_EQ(C.FCSR, 0x800001u);
      EXPECT_FALSE(C.FloatingPointException);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0x12345678u);
    }
  }
}

TEST(MIPSHistorical, EveryConditionCodeComesFromFCSRInBothByteOrders) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Image = historicalImage(Target, 4);
    auto FPU = emulation::createScalarFPExecutor(Target, 2);
    ASSERT_TRUE(bool(FPU)) << llvm::toString(FPU.takeError());
    MIPSLifter L(Target, InstructionMode::Default, 0, 4);
    L.setScalarFPEnabled(true);
    for (unsigned CC = 0; CC < 8; ++CC)
      for (bool Set : {false, true})
        for (bool True : {false, true}) {
          // MOVF/MOVT v0,a0,FCCn. Only the selected condition may affect v0.
          const auto I =
              instruction(0x00801001 | (CC << 18) | (True << 16), Target);
          std::vector<LowOp> Ops;
          L.lift(I, Ops);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(mipsreg::A0, 17);
          E.setRegister(mipsreg::V0, 29);
          MIPSSystemContext C;
          C.ScalarFPU = FPU->get();
          const uint32_t Bit = 1u << (CC ? 24 + CC : 23);
          C.FCSR = Set ? Bit : (0xfe800000u & ~Bit);
          for (const auto &Op : Ops) {
            if (Op.Opcode == NdOp::INTRINSIC &&
                Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSReadFPControl)) {
              ASSERT_EQ(Op.Inputs[1].Offset, 31u);
              ASSERT_TRUE(E.stepMIPSSystem(Op, C));
            } else
              ASSERT_TRUE(E.step(Op));
          }
          EXPECT_EQ(E.getRegister(mipsreg::V0), Set == True ? 17u : 29u);
        }
  }
}
#endif
} // namespace
} // namespace neverd

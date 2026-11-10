//===- MIPS16E2Tests.cpp - Explicit e2 feature and scalar semantics -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// GNU literal encodings and manual-derived results. The pinned Unicorn does
/// not implement e2 and is deliberately not an original-instruction oracle.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace neverd;
namespace {
class MIPS16E2 : public testing::TestWithParam<Arch> {};
BinaryImage e2Image(Arch Target) {
  BinaryImage I;
  I.Arch = Target;
  I.Bits = Bitness::Bits32;
  I.Mode = InstructionMode::MIPS16;
  I.Entry = 0x1000;
  I.MIPS.emplace();
  I.MIPS->HasMIPS16 = I.MIPS->HasMIPS16E2 = true;
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 1024;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned N = 0; N < 1024; ++N)
    Data.Data.push_back(0x81 + N * 3);
  I.Segments.push_back(std::move(Data));
  return I;
}
std::array<uint8_t, 4> e2Bytes(uint32_t Word, Arch Target) {
  std::array<uint8_t, 4> B;
  for (unsigned I = 0; I < 2; ++I) {
    uint16_t H = Word >> (I ? 0 : 16);
    B[2 * I] = H >> (archLittleEndian(Target) ? 0 : 8);
    B[2 * I + 1] = H >> (archLittleEndian(Target) ? 8 : 0);
  }
  return B;
}
std::vector<LowOp> e2Lift(Decoder &D, Arch Target, uint32_t Word) {
  auto B = e2Bytes(Word, Target);
  DecodedInsn I;
  EXPECT_EQ(D.decodeOneForLift(B.data(), B.size(), 0x1002, I), 4);
  std::vector<LowOp> Ops;
  D.liftToLow(I, Ops);
  return Ops;
}
unsigned compact(unsigned R) { return R < 2 ? R + 16 : R; }
bool run(NdOpEmulator &E, llvm::ArrayRef<LowOp> Ops) {
  for (const auto &Op : Ops)
    if (!E.step(Op))
      return false;
  return true;
}
TEST_P(MIPS16E2, ImmediateLiteralsAndEveryDestinationPreserveUnsignedBits) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  const struct {
    uint32_t Word, Expected;
  } Cases[] = {{0xf7ff023f, 0x21ff},
               {0xf0106a61, 0x8001},
               {0xf0106a41, 0x89abcdef},
               {0xf0106a81, 0x89ab4dee},
               {0xf0106a21, 0x80010000}};
  for (const auto &C : Cases)
    for (unsigned X = 0; X < 8; ++X) {
      NdOpEmulator E(I);
      E.setStrictMode(true);
      E.setRegister(mipsreg::r(compact(X)), 0x89abcdef);
      E.setRegister(mipsreg::GP, 0x2200);
      ASSERT_TRUE(run(E, e2Lift(D, GetParam(), (C.Word & ~0x700u) | (X << 8))));
      EXPECT_EQ(E.getRegister(mipsreg::r(compact(X))), C.Expected);
    }
}
TEST_P(MIPS16E2, GPRelativeMemoryHasExactWidthAndTargetByteOrder) {
  const struct {
    uint32_t Word;
    int Offset;
    unsigned Bytes;
    bool Store, Signed;
  } Cases[] = {
      {0xf7ff927f, -1, 1, false, true},  {0xf7ff92bf, -1, 1, false, false},
      {0xf7ff925e, -2, 2, false, true},  {0xf7ff929e, -2, 2, false, false},
      {0xf7ff923c, -4, 4, false, false}, {0xf7ffd27f, -1, 1, true, false},
      {0xf7ffd25e, -2, 2, true, false},  {0xf7ffd23c, -4, 4, true, false}};
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (const auto &C : Cases)
    for (unsigned X = 0; X < 8; ++X) {
      auto Expected = I.Segments[0].Data;
      const unsigned At = 516 + C.Offset;
      uint32_t Value = 0;
      for (unsigned N = 0; N < C.Bytes; ++N) {
        const unsigned Shift =
            8 * (archLittleEndian(GetParam()) ? N : C.Bytes - 1 - N);
        if (C.Store)
          Expected[At + N] = 0x89abcdefu >> Shift;
        else
          Value |= uint32_t(Expected[At + N]) << Shift;
      }
      if (C.Signed && C.Bytes < 4 && (Value & (1u << (C.Bytes * 8 - 1))))
        Value |= ~0u << (C.Bytes * 8);
      NdOpEmulator E(I);
      E.setStrictMode(true);
      E.setRegister(mipsreg::GP, 0x2204);
      E.setRegister(mipsreg::r(compact(X)), 0x89abcdef);
      ASSERT_TRUE(run(E, e2Lift(D, GetParam(), (C.Word & ~0x700u) | (X << 8))));
      if (!C.Store)
        EXPECT_EQ(E.getRegister(mipsreg::r(compact(X))), Value);
      for (unsigned N = At - 1; N <= At + C.Bytes; ++N) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::tmp(0x9000, 1);
        Read.addInput(NdVar::cst(0x2000 + N, 4));
        ASSERT_TRUE(E.step(Read));
        EXPECT_EQ(E.getRegister(0x9000), Expected[N]);
      }
    }
}
TEST_P(MIPS16E2, MergeLiteralsAndAllAlignmentsHaveIndependentByteResults) {
  const struct {
    uint32_t Word;
    int Offset;
    bool Left, Store;
  } Cases[] = {{0xf1e492ff, -1, true, false},
               {0xf0f492ff, 255, false, false},
               {0xf104d2e0, -256, true, true},
               {0xf014d2e0, 0, false, true}};
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  const bool Little = archLittleEndian(GetParam());
  for (const auto &C : Cases)
    for (unsigned Offset = 0; Offset < 4; ++Offset) {
      const unsigned Address = 0x2100 + C.Offset + Offset;
      const unsigned Byte = Address & 3;
      const unsigned Count = C.Left == Little ? Byte + 1 : 4 - Byte;
      const unsigned First = C.Left == Little ? Address - Byte : Address;
      auto Expected = I.Segments[0].Data;
      uint32_t Value = 0x89abcdef;
      for (unsigned N = 0; N < Count; ++N) {
        unsigned Lane = (C.Left ? 4 - Count : 0) + (Little ? N : Count - 1 - N);
        if (C.Store)
          Expected[First - 0x2000 + N] = Value >> (8 * Lane);
        else
          Value = (Value & ~(255u << (8 * Lane))) |
                  (uint32_t(Expected[First - 0x2000 + N]) << (8 * Lane));
      }
      NdOpEmulator E(I);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, 0x2100 + Offset);
      E.setRegister(mipsreg::V0, 0x89abcdef);
      ASSERT_TRUE(run(E, e2Lift(D, GetParam(), C.Word)));
      EXPECT_EQ(E.getRegister(mipsreg::V0), Value);
      for (unsigned N = First - 0x2000; N < First - 0x2000 + Count; ++N) {
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::tmp(0x9000, 1);
        Read.addInput(NdVar::cst(0x2000 + N, 4));
        ASSERT_TRUE(E.step(Read));
        EXPECT_EQ(E.getRegister(0x9000), Expected[N]);
      }
    }
}
TEST_P(MIPS16E2, BitfieldBoundsZeroSourcesAndAliases) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (unsigned Kind = 0; Kind < 3; ++Kind)
    for (unsigned Position = 0; Position < 32; ++Position)
      for (unsigned Size = 1; Size <= 32 - Position; ++Size)
        for (bool Alias : {false, true}) {
          const unsigned Limit = Kind ? Position + Size - 1 : Size - 1;
          const unsigned Source = Kind == 2 ? 0 : Alias ? 2 : 4;
          const uint16_t Prefix =
              0xf000 | (Position << 6) | (Kind == 2 ? 0 : 32) | Limit;
          const uint16_t Suffix =
              0x3000 | (Source << 8) | (2 << 5) | (Kind ? 4 : 8);
          NdOpEmulator E(I);
          E.setStrictMode(true);
          E.setRegister(mipsreg::V0, 0x89abcdef);
          E.setRegister(mipsreg::A0, 0x12345678);
          ASSERT_TRUE(
              run(E, e2Lift(D, GetParam(), (uint32_t(Prefix) << 16) | Suffix)));
          const uint32_t Mask = uint32_t((UINT64_C(1) << Size) - 1);
          const uint32_t Value = Kind == 2 ? 0
                                 : Alias   ? 0x89abcdef
                                           : 0x12345678;
          const uint32_t Expected = Kind ? (0x89abcdef & ~(Mask << Position)) |
                                               ((Value & Mask) << Position)
                                         : (Value >> Position) & Mask;
          EXPECT_EQ(E.getRegister(mipsreg::V0), Expected);
        }
}
TEST_P(MIPS16E2,
       ConditionalMovesCaptureAliasesAndPreserveUnselectedDestination) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Nonzero : {false, true})
    for (bool T : {false, true})
      for (bool Zero : {false, true})
        for (unsigned X = 0; X < 8; ++X)
          for (unsigned Y = 0; Y < 8; ++Y)
            for (unsigned R = 0; R < 8; ++R)
              for (unsigned Condition : {0u, 5u}) {
                const unsigned RX = compact(X), RY = T ? 24 : compact(Y),
                               RB = compact(R);
                const uint16_t Prefix = 0xf000 | (Zero ? 0 : 32 | R);
                const uint16_t Suffix = 0x3000 | (X << 8) | (T ? 0 : Y << 5) |
                                        (T ? 16 : 0) | (Nonzero ? 10 : 6);
                NdOpEmulator E(I);
                E.setStrictMode(true);
                E.setRegister(mipsreg::r(RX), 0x89abcdef);
                E.setRegister(mipsreg::r(RB), 0x12345678);
                E.setRegister(mipsreg::r(RY), Condition);
                const uint32_t Old = *E.getRegister(mipsreg::r(RX));
                const uint32_t Value =
                    Zero ? 0 : *E.getRegister(mipsreg::r(RB));
                ASSERT_TRUE(run(E, e2Lift(D, GetParam(),
                                          (uint32_t(Prefix) << 16) | Suffix)));
                EXPECT_EQ(E.getRegister(mipsreg::r(RX)),
                          (bool(Condition) == Nonzero) ? Value : Old);
              }
}
TEST_P(MIPS16E2,
       FeatureEvidenceRefreshesAndReservedFieldsRefuseWithoutEffects) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Enabled : {true, false, true}) {
    I.MIPS->HasMIPS16E2 = Enabled;
    ASSERT_TRUE(D.selectMode(I, I.Entry));
    const uint32_t Words[] = {
#define MIPS16E2_LITERAL(Word) Word,
#include "MIPS16E2Literals.def"
    };
    for (uint32_t Word : Words) {
      if (Enabled)
        EXPECT_NO_THROW(e2Lift(D, GetParam(), Word));
      else
        EXPECT_THROW(e2Lift(D, GetParam(), Word), UnliftedInstruction);
    }
  }
  MIPSLifter L(GetParam(), InstructionMode::MIPS16);
  L.setMIPS16E2Enabled(true);
  for (uint32_t Word :
       {0xf00832a6u, 0xf01032a6u, 0xf04032a6u, 0xf10032a6u, 0xf20032a6u,
        0xf40032a6u, 0xf00132a6u, 0xf00232a6u, 0xf00432a6u, 0xf0243236u,
        0xf0243256u, 0xf0243296u, 0xf0cb3144u, 0xf0cb3244u, 0xf0cb3444u,
        0xf7e13448u, 0xf1ec92ffu, 0xf3e492ffu, 0xf5e492ffu, 0xf00092c0u}) {
    cs_insn Insn{};
    Insn.size = 4;
    Insn.address = 0x1000;
    auto B = e2Bytes(Word, GetParam());
    std::copy(B.begin(), B.end(), Insn.bytes);
    std::vector<LowOp> Ops(1);
    EXPECT_THROW(L.lift(Insn, Ops), UnliftedInstruction);
    EXPECT_EQ(Ops.size(), 1u);
  }
  auto B = e2Bytes(0xf0243216, GetParam());
  DecodedInsn Insn;
  for (unsigned Size = 0; Size < 4; ++Size)
    EXPECT_EQ(D.decodeOneForLift(B.data(), Size, 0x1000, Insn), 0);
}
TEST_P(MIPS16E2, BarriersKeepAllStypesAndNeverBecomePureOrAlignmentNops) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (unsigned Kind = 0; Kind != 33; ++Kind) {
    const uint32_t Word = Kind == 32 ? 0xf0c03010 : 0xf0003014 | (Kind << 22);
    auto Ops = e2Lift(D, GetParam(), Word);
    ASSERT_EQ(Ops.size(), 1u);
    const auto Id = Kind == 32 ? Intrinsic::MIPSExecutionHazardBarrier
                               : Intrinsic::MIPSSync;
    ASSERT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
    EXPECT_EQ(Ops[0].Inputs[0].Offset, uint64_t(Id));
    EXPECT_EQ(Ops[0].NumInputs, Kind == 32 ? 1u : 2u);
    if (Kind < 32)
      EXPECT_EQ(Ops[0].Inputs[1].Offset, Kind);
    EXPECT_TRUE(isSideeffectIntrinsic(Id));
    EXPECT_FALSE(mips::isPureScalarValue(Id));
    NdOpEmulator E(I);
    E.setStrictMode(true);
    EXPECT_TRUE(run(E, Ops));
    if (Kind < 32) {
      for (unsigned Bad = 0; Bad != 4; ++Bad) {
        auto Op = Ops[0];
        if (Bad == 0)
          Op.Inputs[1] = NdVar::cst(32, 1);
        if (Bad == 1)
          Op.Inputs[1] = NdVar::reg(mipsreg::A0, 1);
        if (Bad == 2)
          Op.Inputs[1].Size = 4;
        if (Bad == 3)
          Op.Output = NdVar::tmp(1, 4);
        EXPECT_FALSE(mipsScalarLowShapeIsValid(Op, GetParam()));
        EXPECT_FALSE(E.step(Op));
      }
    }
  }
  MIPSLifter L(GetParam(), InstructionMode::MIPS16);
  L.setMIPS16E2Enabled(true);
  for (uint32_t Word : {0xf0c03011u, 0xf0e03010u, 0xf0803010u, 0xf0013014u,
                        0xf0203014u, 0xf0003114u, 0xf0003034u}) {
    cs_insn Raw{};
    Raw.size = 4;
    const auto B = e2Bytes(Word, GetParam());
    std::copy(B.begin(), B.end(), Raw.bytes);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    MIPSLifter Classic(Target);
    for (uint32_t Word : {0x000000c0u, 0x0000000fu, 0x000007cfu}) {
      cs_insn Raw{};
      Raw.size = 4;
      for (unsigned N = 0; N != 4; ++N)
        Raw.bytes[N] = Word >> (8 * (archLittleEndian(Target) ? N : 3 - N));
      EXPECT_FALSE(Classic.isAlignmentNop(Raw));
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(Classic.lift(Raw, Ops));
      ASSERT_EQ(Ops.size(), 1u);
      EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(BothByteOrders, MIPS16E2,
                         testing::Values(Arch::MIPS32, Arch::MIPS32EL));
} // namespace

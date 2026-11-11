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
#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/MIPS/MIPSCachePlatform.h"
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
        0xf7e13448u, 0xf1ec92ffu, 0xf3e492ffu, 0xf5e492ffu, 0xf01892c0u,
        0xf60492c0u, 0xf00430ccu, 0xf006674cu}) {
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

LowOp stateOp(Intrinsic Id, NdVar Output,
              std::initializer_list<NdVar> Inputs = {}) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = Output;
  Op.addInput(NdVar::cst(uint64_t(Id), sizeof(Intrinsic)));
  for (auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp ordinaryStore(uint32_t Address, uint64_t Value, uint16_t Bytes = 4) {
  LowOp Op;
  Op.Opcode = NdOp::STORE;
  Op.addInput(NdVar::cst(Address, 4));
  Op.addInput(NdVar::cst(Value, Bytes));
  return Op;
}
LowOp linked(uint32_t Address) {
  return stateOp(Intrinsic::MIPSLoadLinkedWord, NdVar::reg(mipsreg::V0, 4),
                 {NdVar::cst(Address, 4)});
}
LowOp conditional(uint32_t Address, NdVar Data = NdVar::cst(0xdeadbeef, 4)) {
  return stateOp(Intrinsic::MIPSStoreConditionalWord,
                 NdVar::reg(mipsreg::V1, 4), {NdVar::cst(Address, 4), Data});
}

TEST_P(MIPS16E2, LinkedWordsCaptureEveryCompactBaseDestinationAndSignedOffset) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Strict : {false, true})
    for (unsigned X = 0; X != 8; ++X)
      for (unsigned B = 0; B != 8; ++B)
        for (int Offset : {-256, -4, 0, 252, 255}) {
          const auto Word =
              uint32_t(0xf0000000 | ((uint32_t(Offset) & 0x1e0) << 16) |
                       (B << 16) | 0x90c0 | (X << 8) | (uint32_t(Offset) & 31));
          auto Load = e2Lift(D, GetParam(), Word);
          auto Store = e2Lift(D, GetParam(), Word | 0x4000);
          NdOpEmulator E(I);
          E.setStrictMode(Strict);
          E.setRegister(mipsreg::r(compact(B)), uint32_t(0x2100 - Offset));
          EXPECT_FALSE(run(E, Load));
          ASSERT_TRUE(E.setMIPSReservationModel(16));
          ASSERT_TRUE(run(E, Load));
          uint32_t Expected = 0;
          for (unsigned N = 0; N != 4; ++N)
            Expected |= uint32_t(I.Segments[0].Data[0x100 + N])
                        << (8 * (archLittleEndian(GetParam()) ? N : 3 - N));
          EXPECT_EQ(E.getRegister(mipsreg::r(compact(X))), Expected);
          E.setRegister(mipsreg::r(compact(B)), uint32_t(0x2100 - Offset));
          if (X != B)
            E.setRegister(mipsreg::r(compact(X)), 0x12345678);
          const auto Stored = X == B ? uint32_t(0x2100 - Offset) : 0x12345678u;
          ASSERT_TRUE(run(E, Store));
          EXPECT_EQ(E.getRegister(mipsreg::r(compact(X))), 1u);
          ASSERT_TRUE(E.step(linked(0x2100)));
          EXPECT_EQ(E.getRegister(mipsreg::V0), Stored);
        }
}

TEST_P(MIPS16E2, ReservationTracksGranulesABAReplacementAndKnownFailure) {
  auto I = e2Image(GetParam());
  for (bool Strict : {false, true})
    for (uint32_t Granule : {4u, 16u, 64u}) {
      NdOpEmulator E(I);
      E.setStrictMode(Strict);
      for (uint32_t Invalid : {0u, 1u, 2u, 3u, 6u})
        EXPECT_FALSE(E.setMIPSReservationModel(Invalid));
      ASSERT_TRUE(E.setMIPSReservationModel(Granule));
      EXPECT_FALSE(E.step(conditional(0x2000)));
      E.clearMIPSReservation();
      ASSERT_TRUE(E.step(conditional(0x2000, NdVar::reg(mipsreg::A3, 4))));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      ASSERT_TRUE(E.step(linked(0x2000)));
      const auto Old = *E.getRegister(mipsreg::V0);
      // A load within the same block and an outside store in the declared
      // no-eviction profile do not invalidate the reservation.
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::reg(mipsreg::A2, 1);
      Load.addInput(NdVar::cst(0x2000 + Granule - 1, 4));
      ASSERT_TRUE(E.step(Load));
      ASSERT_TRUE(E.step(ordinaryStore(0x2000 + Granule, 17)));
      ASSERT_TRUE(E.step(conditional(0x2000)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
      ASSERT_TRUE(E.step(conditional(0x2000)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      // Same-block byte store invalidates even if the linked word is intact.
      ASSERT_TRUE(E.step(linked(0x2000)));
      ASSERT_TRUE(E.step(ordinaryStore(0x2000 + Granule - 1, 17, 1)));
      ASSERT_TRUE(E.step(conditional(0x2000)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      // Restoring exactly the original word does not revive a reservation.
      ASSERT_TRUE(E.step(linked(0x2000)));
      ASSERT_TRUE(E.step(ordinaryStore(0x2000, 0)));
      ASSERT_TRUE(E.step(ordinaryStore(0x2000, Old)));
      ASSERT_TRUE(E.step(conditional(0x2000)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      // A new LL replaces rather than accumulates outstanding addresses.
      ASSERT_TRUE(E.step(linked(0x2000)));
      ASSERT_TRUE(E.step(linked(0x2004)));
      ASSERT_TRUE(E.step(conditional(0x2000, NdVar::reg(mipsreg::A3, 4))));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      ASSERT_TRUE(E.step(conditional(0x2004)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
      ASSERT_TRUE(E.step(linked(0x2000)));
      EXPECT_FALSE(E.step(conditional(0x2000, NdVar::reg(mipsreg::A3, 4))));
      ASSERT_TRUE(E.step(conditional(0x2000)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
    }
}

TEST_P(MIPS16E2, ReservationRefusesInvalidAccessBeforeEffectInBothModes) {
  for (bool Strict : {false, true}) {
    auto I = e2Image(GetParam());
    // Adjacent read-only segment and wholly unmapped memory exercise both
    // permissions even when a lenient prior write has populated the overlay.
    Segment RO = I.Segments[0];
    RO.VA = 0x3000;
    RO.Flags = SegmentFlags::Readable;
    I.Segments.push_back(RO);
    NdOpEmulator E(I);
    E.setStrictMode(false);
    ASSERT_TRUE(E.step(ordinaryStore(0x3000, 9)));
    ASSERT_TRUE(E.step(ordinaryStore(0x4000, 11)));
    E.setStrictMode(Strict);
    ASSERT_TRUE(E.setMIPSReservationModel(16));
    ASSERT_TRUE(E.step(linked(0x2000)));
    E.setRegister(mipsreg::V1, 73);
    for (uint32_t Bad : {0x2001u, 0x3000u, 0x4000u}) {
      EXPECT_FALSE(E.step(conditional(Bad)));
      EXPECT_EQ(E.getRegister(mipsreg::V1), 73u);
    }
    EXPECT_FALSE(E.step(linked(0x4000)));
    EXPECT_FALSE(E.step(linked(0x2001)));
    auto Invalid = conditional(0x2000);
    Invalid.Inputs[2].Size = 8;
    EXPECT_FALSE(E.step(Invalid));
    Invalid = conditional(0x2000);
    Invalid.Output = NdVar::cst(0, 4);
    EXPECT_FALSE(E.step(Invalid));
    Invalid = conditional(0x2000);
    Invalid.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    EXPECT_FALSE(E.step(Invalid));
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
    E.clearMIPSReservation();
    EXPECT_FALSE(
        E.step(conditional(0x3000))); // failed-SC still needs writable mapping
    ASSERT_TRUE(
        E.step(linked(0x3000))); // readable mapping is sufficient for LL
    EXPECT_EQ(E.getRegister(mipsreg::V0), 9u);
    EXPECT_FALSE(E.step(conditional(0x3000)));
    I.Segments[0].Flags = SegmentFlags::Writable;
    EXPECT_FALSE(
        E.step(linked(0x2000))); // overlay cannot bypass read permissions
  }
}

TEST_P(MIPS16E2, UnknownEffectsAndCallsForgetReservationsUntilAnotherLL) {
  auto I = e2Image(GetParam());
  NdOpEmulator E(I);
  E.setCallPreservedRegisters({mipsreg::A0});
  ASSERT_TRUE(E.setMIPSReservationModel(16));
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    ASSERT_TRUE(E.step(linked(0x2000)));
    LowOp Unknown;
    if (Kind == 0) {
      Unknown.Opcode = NdOp::CALL;
      Unknown.addInput(NdVar::cst(0x1234, 4));
      ASSERT_TRUE(E.step(Unknown));
    } else if (Kind == 1) {
      Unknown.Opcode = NdOp::INTRINSIC;
      Unknown.addInput(
          NdVar::cst(uint64_t(Intrinsic::_Count) - 1, sizeof(Intrinsic)));
      ASSERT_TRUE(E.step(Unknown));
    } else if (Kind == 2) {
      Unknown.Opcode = static_cast<NdOp>(0xffff);
      ASSERT_TRUE(E.step(Unknown));
    } else
      E.reset();
    E.setRegister(mipsreg::V1, 91);
    EXPECT_FALSE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 91u);
    ASSERT_TRUE(E.step(linked(0x2000)));
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
  }
}

TEST_P(MIPS16E2,
       ReservationProfileNeverManufacturesUnknownInputsOrForeignEffects) {
  for (bool Strict : {false, true}) {
    auto I = e2Image(GetParam());
    Segment Zero = I.Segments[0];
    Zero.VA = 0;
    I.Segments.push_back(Zero);
    Decoder D;
    ASSERT_TRUE(D.init(I));
    NdOpEmulator E(I);
    E.setStrictMode(Strict);
    ASSERT_TRUE(E.setMIPSReservationModel(16));
    ASSERT_TRUE(E.step(linked(0x2000)));
    const auto Before = E.getRegister(mipsreg::V0);
    EXPECT_FALSE(run(E, e2Lift(D, GetParam(),
                               0xf00492c0))); // missing base must not become 0
    EXPECT_EQ(E.getRegister(mipsreg::V0), Before);
    auto Store = ordinaryStore(0x2000, 11);
    Store.Inputs[0] = NdVar::reg(mipsreg::A2, 4);
    EXPECT_FALSE(E.step(Store));
    E.setRegister(mipsreg::A2, 0x2000);
    Store.Inputs[1] = NdVar::reg(mipsreg::A3, 4);
    EXPECT_FALSE(E.step(Store));
    ASSERT_TRUE(E.step(
        stateOp(Intrinsic::MIPSUnpredictable, NdVar::reg(mipsreg::A3, 4))));
    EXPECT_FALSE(E.step(Store));
    ASSERT_TRUE(
        E.step(stateOp(Intrinsic::MIPSLwl, NdVar::reg(mipsreg::A3, 4),
                       {NdVar::cst(0x2001, 4), NdVar::reg(mipsreg::A3, 4)})));
    LowOp Transport;
    Transport.Opcode = NdOp::COPY;
    Transport.Output = NdVar::reg(mipsreg::A0, 4);
    Transport.addInput(NdVar::reg(mipsreg::A3, 4));
    EXPECT_FALSE(E.step(Transport)); // conservative ordinary-input profile
    std::vector<uint8_t> Bytes(16, 255);
    E.setRegisterBytes(mipsreg::w(0), Bytes);
    E.setRegisterBytes(mipsreg::w(1), Bytes);
    auto Foreign =
        stateOp(Intrinsic::MaskedStoreD, {},
                {NdVar::cst(0x2000, 4), NdVar::reg(mipsreg::w(0), 16),
                 NdVar::reg(mipsreg::w(1), 16)});
    EXPECT_FALSE(E.step(Foreign));
    auto Align = stateOp(Intrinsic::RequireAligned, {},
                         {NdVar::cst(0x2000, 8), NdVar::scalar(4, 8)});
    ASSERT_TRUE(E.step(Align));
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
    ASSERT_TRUE(E.step(linked(0x2000)));
    E.setRegister(mipsreg::GP, 0x2000);
    E.setRegister(mipsreg::A0, 7);
    ASSERT_TRUE(
        run(E, e2Lift(D, GetParam(), 0xf000d420))); // ordinary aligned SW
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
  }
}

TEST_P(MIPS16E2, ASkippedBudgetedStoreCannotAuthenticateConditionalSuccess) {
  auto I = e2Image(GetParam());
  for (bool Strict : {false, true}) {
    NdOpEmulator E(I);
    E.setStrictMode(Strict);
    ASSERT_TRUE(E.setMIPSReservationModel(16));
    ASSERT_TRUE(E.step(ordinaryStore(0x2000, 11)));
    for (int N = 1; N < limits::kMaxEmulatorStoreEntries; ++N)
      ASSERT_TRUE(E.step(ordinaryStore(0x2100 + 4 * N, 17)));
    ASSERT_TRUE(E.step(linked(0x2000)));
    EXPECT_EQ(E.step(ordinaryStore(0x2004, 9)), !Strict);
    EXPECT_EQ(E.step(conditional(0x2000)), Strict);
    ASSERT_TRUE(E.step(linked(0x2000)));
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
  }
}

TEST_P(MIPS16E2,
       SystemReadsConsumeOneAuthenticatedObservationAndHonorPermissions) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Strict : {false, true})
    for (unsigned Index : {0u, 1u, 2u, 3u, 29u, 30u, 31u})
      for (unsigned RY = 0; RY != 8; ++RY)
        for (bool CP0 : {false, true}) {
          NdOpEmulator E(I);
          E.setStrictMode(Strict);
          auto Ops =
              e2Lift(D, GetParam(), 0xf000300c | (Index << 16) | (RY << 5));
          ASSERT_EQ(Ops.size(), 2u);
          EXPECT_FALSE(E.step(Ops[0]));
          MIPSSystemContext C;
          C.Hardware = MIPSHardwareObservation{uint8_t(Index), 0xabcdef01};
          EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
          ASSERT_TRUE(C.Hardware);
          C.HWREna = uint32_t(1) << ((Index + 1) % 32);
          EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
          ASSERT_TRUE(C.Hardware);
          C.CP0Usable = CP0;
          C.HWREna = CP0 ? 0 : uint32_t(1) << Index;
          C.ExecutionHazardPending = true;
          EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
          ASSERT_TRUE(E.stepMIPSSystem(
              stateOp(Intrinsic::MIPSExecutionHazardBarrier, {}), C));
          C.Hardware->Index = (Index + 1) % 32;
          EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
          C.Hardware->Index = Index;
          ASSERT_TRUE(E.stepMIPSSystem(Ops[0], C));
          ASSERT_TRUE(E.step(Ops[1]));
          EXPECT_EQ(E.getRegister(mipsreg::r(compact(RY))), 0xabcdef01u);
          EXPECT_FALSE(C.Hardware);
          EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
        }
}

TEST_P(MIPS16E2, InterruptUpdatesReturnOldStatusAndRequireAnExecutionBarrier) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Enable : {false, true})
    for (bool Discard : {false, true})
      for (uint32_t Status : {0u, 1u, 0xfffffffeu, 0xffffffffu}) {
        auto Ops = e2Lift(D, GetParam(),
                          0xf002670c | (uint32_t(Enable) << 16) |
                              (Discard ? 0x40000 : 2 << 5));
        NdOpEmulator E(I);
        E.setRegister(mipsreg::V0, 17);
        MIPSSystemContext C;
        EXPECT_FALSE(E.step(Ops[0]));
        EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
        C.Status = Status;
        EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
        EXPECT_EQ(C.Status, Status);
        C.Status.reset();
        C.CP0Usable = true;
        EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
        C.Status = Status;
        ASSERT_TRUE(E.stepMIPSSystem(Ops[0], C));
        for (unsigned N = 1; N < Ops.size(); ++N)
          ASSERT_TRUE(E.step(Ops[N]));
        EXPECT_EQ(E.getRegister(mipsreg::V0), Discard ? 17u : Status);
        EXPECT_EQ(C.Status, (Status & ~1u) | unsigned(Enable));
        EXPECT_TRUE(C.ExecutionHazardPending);
        EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
        auto Sync = stateOp(Intrinsic::MIPSSync, {}, {NdVar::cst(0, 1)});
        ASSERT_TRUE(E.step(Sync));
        EXPECT_FALSE(E.stepMIPSSystem(Sync, C));
        EXPECT_TRUE(C.ExecutionHazardPending);
        auto EHB = stateOp(Intrinsic::MIPSExecutionHazardBarrier, {});
        ASSERT_TRUE(E.step(EHB));
        EXPECT_TRUE(
            C.ExecutionHazardPending); // unbound step cannot alter caller state
        ASSERT_TRUE(E.stepMIPSSystem(EHB, C));
        EXPECT_FALSE(C.ExecutionHazardPending);
        ASSERT_TRUE(E.stepMIPSSystem(Ops[0], C));
      }
  for (unsigned Index : {4u, 5u, 6u, 28u}) {
    auto Invalid = stateOp(Intrinsic::MIPSReadHardwareRegister,
                           NdVar::reg(mipsreg::V0, 4), {NdVar::cst(Index, 1)});
    NdOpEmulator E(I);
    MIPSSystemContext C;
    C.CP0Usable = true;
    C.Hardware = MIPSHardwareObservation{uint8_t(Index), 13};
    EXPECT_FALSE(E.stepMIPSSystem(Invalid, C));
    EXPECT_TRUE(C.Hardware);
    EXPECT_THROW(e2Lift(D, GetParam(), 0xf000304c | (Index << 16)),
                 UnliftedInstruction);
  }
}

TEST_P(MIPS16E2, CP0WordViewsPreserveAllBitsAndRequireBoundPrivilege) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Strict : {false, true})
    for (unsigned RX = 0; RX != 8; ++RX)
      for (unsigned Index : {12u, 14u, 30u})
        for (uint32_t Value : {0u, 0x1001u, 0x1003u, 0xffffffffu}) {
          NdOpEmulator E(I);
          E.setStrictMode(Strict);
          MIPSSystemContext C;
          C.HWREna = 0xffffffff;
          C.Status = 0x12345678;
          C.EPC = 0x87654321;
          C.ErrorEPC = 0x13572468;
          auto &View = Index == 12   ? C.Status
                       : Index == 14 ? C.EPC
                                     : C.ErrorEPC;
          View = Value;
          auto Read = e2Lift(D, GetParam(), 0xf0006700 | (RX << 5) | Index);
          EXPECT_FALSE(E.step(Read[0]));
          EXPECT_FALSE(E.stepMIPSSystem(Read[0], C)); // HWREna is insufficient.
          C.CP0Usable = true;
          View.reset();
          EXPECT_FALSE(E.stepMIPSSystem(Read[0], C));
          View = Value;
          ASSERT_TRUE(E.stepMIPSSystem(Read[0], C));
          ASSERT_TRUE(run(E, llvm::ArrayRef(Read).drop_front()));
          EXPECT_EQ(E.getRegister(mipsreg::r(compact(RX))), Value);
          EXPECT_FALSE(C.ExecutionHazardPending);
          if (Index == 12)
            continue;
          auto Write = e2Lift(D, GetParam(), 0xf0016700 | (RX << 5) | Index);
          EXPECT_FALSE(E.step(Write[0]));
          C.CP0Usable = false;
          EXPECT_FALSE(E.stepMIPSSystem(Write[0], C));
          EXPECT_EQ(View, Value);
          C.CP0Usable = true;
          auto Missing = Write[0];
          Missing.Inputs[2] = NdVar::reg(mipsreg::r(8), 4);
          EXPECT_FALSE(E.stepMIPSSystem(Missing, C));
          EXPECT_EQ(View, Value);
          View.reset(); // A write establishes this previously unknown view.
          ASSERT_TRUE(E.stepMIPSSystem(Write[0], C));
          EXPECT_EQ(View, Value);
          EXPECT_TRUE(C.ExecutionHazardPending);
          EXPECT_FALSE(E.stepMIPSSystem(Read[0], C));
          EXPECT_FALSE(E.stepMIPSSystem(Write[0], C));
          ASSERT_TRUE(E.stepMIPSSystem(
              stateOp(Intrinsic::MIPSExecutionHazardBarrier, {}), C));
          ASSERT_TRUE(E.stepMIPSSystem(Read[0], C));
          ASSERT_TRUE(run(E, llvm::ArrayRef(Read).drop_front()));
          EXPECT_EQ(E.getRegister(mipsreg::r(compact(RX))), Value);
          EXPECT_EQ(C.Status, 0x12345678u);
          EXPECT_EQ(Index == 14 ? C.ErrorEPC : C.EPC,
                    Index == 14 ? 0x13572468u : 0x87654321u);
          EXPECT_EQ(C.HWREna, 0xffffffffu);
          EXPECT_EQ(I.Mode, InstructionMode::MIPS16);
        }
}
TEST_P(MIPS16E2, CP0PlatformHonorsWriteMasksAndSharedStatus) {
  struct Platform final : MIPSCP0Platform {
    std::optional<uint32_t> readWord(uint8_t Key,
                                     MIPSSystemContext &Context) override {
      return Key == 0xfd ? Context.Status : std::nullopt;
    }
    bool writeWord(uint8_t Key, uint32_t Value,
                   MIPSSystemContext &Context) override {
      if (Key != 12 || !Context.Status)
        return false;
      Context.Status = (*Context.Status & ~3u) | (Value & 3);
      return true;
    }
  } Board;
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setRegister(mipsreg::V0, 0xfffffff2);
  MIPSSystemContext C;
  C.Status = 0x2135;
  C.CP0Usable = true;
  const auto Write = e2Lift(D, GetParam(), 0xf001674c);
  EXPECT_FALSE(E.stepMIPSSystem(Write[0], C));
  C.CP0 = &Board;
  ASSERT_TRUE(E.stepMIPSSystem(Write[0], C));
  EXPECT_EQ(C.Status, 0x2136u);
  const auto Read = e2Lift(D, GetParam(), 0xf0e0675d);
  EXPECT_FALSE(E.stepMIPSSystem(Read[0], C));
  ASSERT_TRUE(
      E.stepMIPSSystem(stateOp(Intrinsic::MIPSExecutionHazardBarrier, {}), C));
  ASSERT_TRUE(E.stepMIPSSystem(Read[0], C));
  ASSERT_TRUE(run(E, llvm::ArrayRef(Read).drop_front()));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0x2136u);
}
TEST_P(MIPS16E2, CP0SelectorsAndStatefulShapesRejectUnsupportedContracts) {
  auto I = e2Image(GetParam());
  Decoder D;
  for (bool MT : {false, true}) {
    I.MIPS->HasMT = MT;
    ASSERT_TRUE(D.init(I));
    for (unsigned Write = 0; Write != 2; ++Write)
      for (unsigned Selector = 0; Selector != 8; ++Selector)
        for (unsigned Index = 0; Index != 32; ++Index) {
          const uint32_t Word =
              0xf0006740 | (Write << 16) | (Selector << 21) | Index;
          const bool ThreadView =
              (Index == 0 && Selector >= 1 && Selector <= 3) ||
              ((Index == 1 || Index == 2) && Selector != 0) ||
              (Index == 3 && Selector == 7);
          const bool Admitted = MT || !ThreadView;
          if (Admitted) {
            auto Ops = e2Lift(D, GetParam(), Word);
            EXPECT_TRUE(mipsScalarLowShapeIsValid(Ops[0], GetParam()));
            for (unsigned Bad = 0; Bad != 5; ++Bad) {
              auto Op = Ops[0];
              if (Bad == 0)
                Op.Inputs[1] = NdVar::cst(256, 1);
              if (Bad == 1)
                Op.Inputs[1] = NdVar::reg(mipsreg::A0, 1);
              if (Bad == 2)
                Op.Inputs[1].Size = 4;
              if (Bad == 3)
                Op.Output = Write ? NdVar::tmp(999, 4) : NdVar{};
              if (Bad == 4)
                ++Op.NumInputs;
              EXPECT_FALSE(mipsScalarLowShapeIsValid(Op, GetParam()));
            }
            EXPECT_FALSE(mipsScalarLowShapeIsValid(Ops[0], Arch::MIPS64));
          } else
            EXPECT_THROW(e2Lift(D, GetParam(), Word), UnliftedInstruction);
        }
  }
  for (uint32_t Word : {0xf000674cu, 0xf001674eu, 0xf000679eu, 0xf1403018u}) {
    MIPSLifter L(GetParam(), InstructionMode::MIPS16);
    cs_insn Raw{};
    Raw.size = 4;
    auto B = e2Bytes(Word, GetParam());
    std::copy(B.begin(), B.end(), Raw.bytes);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}
TEST_P(MIPS16E2, PauseCompletesOnlyWithExplicitlyKnownClearReservation) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  auto Ops = e2Lift(D, GetParam(), 0xf1403018);
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Ops[0].Inputs[0].Offset, uint64_t(Intrinsic::MIPSPause));
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::MIPSPause));
  EXPECT_FALSE(mips::isBarrier(Intrinsic::MIPSPause));
  for (bool Strict : {false, true}) {
    NdOpEmulator E(I);
    E.setStrictMode(Strict);
    EXPECT_FALSE(run(E, Ops));
    E.clearMIPSReservation();
    EXPECT_FALSE(run(E, Ops));
    ASSERT_TRUE(E.setMIPSReservationModel(16));
    EXPECT_FALSE(run(E, Ops));
    E.clearMIPSReservation();
    ASSERT_TRUE(run(E, Ops));
    ASSERT_TRUE(E.step(linked(0x2000)));
    EXPECT_FALSE(run(E, Ops));
    ASSERT_TRUE(E.step(stateOp(Intrinsic::MIPSExecutionHazardBarrier, {})));
    ASSERT_TRUE(E.step(stateOp(Intrinsic::MIPSSync, {}, {NdVar::cst(0, 1)})));
    EXPECT_FALSE(run(E, Ops));
    ASSERT_TRUE(E.step(conditional(0x2000)));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 1u);
    ASSERT_TRUE(run(E, Ops));
    ASSERT_TRUE(E.step(linked(0x2000)));
    ASSERT_TRUE(E.step(ordinaryStore(0x2004, 17)));
    ASSERT_TRUE(run(E, Ops));
    E.reset();
    EXPECT_FALSE(run(E, Ops));
    E.clearMIPSReservation();
    ASSERT_TRUE(run(E, Ops));
  }
  // Classic PAUSE must not be silently normalized to a discarded SLL.
  MIPSLifter Classic(GetParam());
  cs_insn RawPause{};
  RawPause.size = 4;
  for (unsigned N = 0; N != 4; ++N)
    RawPause.bytes[N] =
        0x140u >> (8 * (archLittleEndian(GetParam()) ? N : 3 - N));
  std::vector<LowOp> Rejected;
  EXPECT_THROW(Classic.lift(RawPause, Rejected), UnliftedInstruction);
  EXPECT_TRUE(Rejected.empty());
  auto Bad = Ops[0];
  Bad.Output = NdVar::tmp(1, 4);
  EXPECT_FALSE(mipsScalarLowShapeIsValid(Bad, GetParam()));
  EXPECT_FALSE(mipsScalarLowShapeIsValid(Ops[0], Arch::MIPS64));
  MIPSLifter L(GetParam(), InstructionMode::MIPS16);
  L.setMIPS16E2Enabled(true);
  cs_insn Head{}, Slot{};
  Head.address = 0x1000;
  Head.size = 2;
  Slot.address = 0x1002;
  Slot.size = 4;
  const auto Jump = e2Bytes(0xe8200000, GetParam()),
             Pause = e2Bytes(0xf1403018, GetParam());
  std::copy_n(Jump.begin(), 2, Head.bytes);
  std::copy(Pause.begin(), Pause.end(), Slot.bytes);
  std::vector<LowOp> Out;
  EXPECT_THROW(L.liftUnit(Head, Slot, Out), UnliftedInstruction);
  EXPECT_TRUE(Out.empty());
}

TEST_P(MIPS16E2, MTControlUsesBoundViewsMasterPermissionAndHazards) {
  auto I = e2Image(GetParam());
  I.MIPS->HasMT = true;
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Strict : {false, true})
    for (unsigned Kind = 0; Kind != 4; ++Kind)
      for (bool Discard : {false, true})
        for (unsigned Y = 0; Y != (Discard ? 1u : 8u); ++Y)
          for (bool Master : {false, true})
            for (uint32_t Old : {0u, 0x13572468u, 0xffffffffu}) {
              const bool Thread = Kind >= 2, Enable = Kind & 1;
              const uint32_t Word = 0xf0226700 | (Enable << 16) |
                                    (Discard << 18) | (Y << 5) | Thread;
              auto Ops = e2Lift(D, GetParam(), Word);
              ASSERT_TRUE(mipsScalarLowShapeIsValid(Ops[0], GetParam()));
              NdOpEmulator E(I);
              E.setStrictMode(Strict);
              MIPSSystemContext C;
              C.Status = 0x55aa;
              C.MVPControl = C.VPEControl = Old;
              C.VPEConf0 = Master ? 2 : 0;
              EXPECT_FALSE(E.step(Ops[0]));
              EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
              C.CP0Usable = true;
              EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
              C.ImplementedMT = true;
              auto &Control = Thread ? C.VPEControl : C.MVPControl;
              Control.reset();
              EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
              EXPECT_FALSE(C.ExecutionHazardPending);
              Control = Old;
              if (!Thread) {
                C.VPEConf0.reset();
                EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
                EXPECT_EQ(Control, Old);
                C.VPEConf0 = Master ? 2 : 0;
              }
              ASSERT_TRUE(E.stepMIPSSystem(Ops[0], C));
              ASSERT_TRUE(run(E, llvm::ArrayRef(Ops).drop_front()));
              if (!Discard)
                EXPECT_EQ(E.getRegister(mipsreg::r(compact(Y))), Old);
              const uint32_t Mask = Thread ? 0x8000 : 1;
              EXPECT_EQ(Control, Thread || Master
                                     ? (Enable ? Old | Mask : Old & ~Mask)
                                     : Old);
              EXPECT_EQ(Thread ? C.MVPControl : C.VPEControl, Old);
              EXPECT_EQ(C.Status, 0x55aau);
              EXPECT_TRUE(C.ExecutionHazardPending);
              EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
              ASSERT_TRUE(E.stepMIPSSystem(
                  stateOp(Intrinsic::MIPSExecutionHazardBarrier, {}), C));
              EXPECT_FALSE(C.ExecutionHazardPending);
            }
  for (unsigned Key : {32u, 33u, 65u}) {
    auto Ops =
        e2Lift(D, GetParam(), 0xf0006740 | ((Key >> 5) << 21) | (Key & 31));
    NdOpEmulator E(I);
    MIPSSystemContext C;
    C.CP0Usable = true;
    C.MVPControl = C.VPEControl = C.VPEConf0 = 0x87654321;
    EXPECT_FALSE(E.stepMIPSSystem(Ops[0], C));
    C.ImplementedMT = true;
    ASSERT_TRUE(E.stepMIPSSystem(Ops[0], C));
    ASSERT_TRUE(run(E, llvm::ArrayRef(Ops).drop_front()));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x87654321u);
    const auto Write =
        e2Lift(D, GetParam(), 0xf0016740 | ((Key >> 5) << 21) | (Key & 31));
    EXPECT_FALSE(E.stepMIPSSystem(Write[0], C));
  }
  I.MIPS->HasMT = false;
  ASSERT_TRUE(D.init(I));
  for (uint32_t Word : {0xf0226741u, 0xf0236741u, 0xf0226740u, 0xf0236740u,
                        0xf0206740u, 0xf0406741u})
    EXPECT_THROW(e2Lift(D, GetParam(), Word), UnliftedInstruction);
}

TEST_P(MIPS16E2, CacheAndPrefetchRetainExactEffectsWithoutInventingExecution) {
  auto I = e2Image(GetParam());
  Decoder D;
  ASSERT_TRUE(D.init(I));
  for (bool Prefetch : {false, true})
    for (unsigned Hint = 0; Hint != 32; ++Hint)
      for (unsigned Base = 0; Base != 8; ++Base)
        for (int Offset : {-256, -1, 0, 31, 255}) {
          const uint32_t Word = 0xf000d080 | ((!Prefetch) << 5) | (Hint << 16) |
                                (Base << 8) | ((Offset & 0x1e0) << 16) |
                                (Offset & 31);
          auto Ops = e2Lift(D, GetParam(), Word);
          ASSERT_EQ(Ops.size(), 2u);
          const auto &Effect = Ops.back();
          const auto Id =
              Prefetch ? Intrinsic::MIPSPrefetch : Intrinsic::MIPSCache;
          EXPECT_EQ(Effect.Inputs[0].Offset, uint64_t(Id));
          EXPECT_EQ(Effect.Inputs[1], NdVar::cst(Hint, 1));
          EXPECT_TRUE(mips::scalarMayWriteMemory(Id));
          EXPECT_TRUE(isSideeffectIntrinsic(Id));
          EXPECT_FALSE(mips::isBarrier(Id));
          ASSERT_TRUE(mipsScalarLowShapeIsValid(Effect, GetParam()));
          EXPECT_FALSE(mipsScalarLowShapeIsValid(Effect, Arch::MIPS64));
          for (bool Strict : {false, true}) {
            NdOpEmulator E(I);
            E.setStrictMode(Strict);
            E.setRegister(mipsreg::r(compact(Base)), 0xfffffff0);
            ASSERT_TRUE(E.step(Ops.front()));
            LowOp Copy;
            Copy.Opcode = NdOp::COPY;
            Copy.Output = NdVar::reg(mipsreg::V1, 4);
            Copy.addInput(Effect.Inputs[2]);
            ASSERT_TRUE(E.step(Copy));
            EXPECT_EQ(E.getRegister(mipsreg::V1),
                      uint32_t(0xfffffff0u + Offset));
            EXPECT_FALSE(E.step(Effect));
            MIPSSystemContext C;
            C.CP0Usable = C.ImplementedMT = true;
            EXPECT_FALSE(E.stepMIPSSystem(Effect, C));
          }
          for (unsigned Bit : {25u, 26u}) {
            auto B = e2Bytes(Word | (1u << Bit), GetParam());
            DecodedInsn Bad;
            EXPECT_EQ(D.decodeOneForLift(B.data(), B.size(), 0x1002, Bad), 0);
          }
        }
}

TEST_P(MIPS16E2,
       CacheUsesDataTranslationAndCommitsDirtyWritebackBeforeInvalidation) {
  struct Board : MIPSCachePlatform {
    std::optional<MIPSCacheTranslation> Translation;
    MIPSCacheLine Line{0x12000, {1, 2, 3, 4}, true, true, true};
    std::vector<uint8_t> Memory;
    uint32_t LastVA = 0, Calls = 0;
    bool Fail = false;
    unsigned Fills = 0;
    std::optional<MIPSCacheTranslation>
    translateDataRead(uint32_t VA) override {
      LastVA = VA;
      return Translation;
    }
    bool apply(const MIPSCacheRequest &R) override {
      ++Calls;
      if (R.Action == MIPSCacheAction::Prefetch)
        return true;
      const auto Base = R.Translation.PhysicalAddress & ~UINT64_C(3);
      MIPSCacheSelection Selected{Line, Base == Line.PhysicalBase, false};
      const auto Write = [&](uint64_t OldBase, const auto &Bytes) {
        EXPECT_EQ(OldBase, 0x12000u);
        if (Fail)
          return false;
        Memory = Bytes;
        return true;
      };
      if (R.Action == MIPSCacheAction::Fill ||
          R.Action == MIPSCacheAction::FetchAndLock)
        return fill(
            R, Selected, Base,
            [&](uint64_t Requested, auto &Bytes) {
              ++Fills;
              EXPECT_EQ(Requested, Base);
              for (size_t I = 0; I < Bytes.size(); ++I)
                Bytes[I] = uint8_t((Requested >> 12) + I);
              return true;
            },
            Write);
      return maintain(R, Selected, Write);
    }
  } B;
  BinaryImage I;
  I.Arch = GetParam();
  NdOpEmulator E(I);
  MIPSSystemContext C;
  C.Cache = &B;
  const auto Cache = [&](unsigned Code, bool Prefetch = false) {
    auto O =
        stateOp(Prefetch ? Intrinsic::MIPSPrefetch : Intrinsic::MIPSCache, {});
    O.Output = {};
    O.addInput(NdVar::cst(Code, 1));
    O.addInput(NdVar::cst(0x80001233, 4));
    return E.stepMIPSSystem(O, C);
  };
  EXPECT_FALSE(Cache(21));
  EXPECT_EQ(B.LastVA, 0u);
  C.CP0Usable = true;
  EXPECT_FALSE(Cache(21));
  B.Translation = MIPSCacheTranslation{MIPSCacheMapping::AddressException, 0};
  EXPECT_FALSE(Cache(21));
  C.CP0Usable = false;
  EXPECT_TRUE(Cache(30, true));
  B.Translation->Mapping = MIPSCacheMapping::Uncached;
  EXPECT_TRUE(Cache(30, true));
  EXPECT_EQ(B.Calls, 0u);
  C.CP0Usable = true;
  EXPECT_FALSE(Cache(21));
  B.Translation->Mapping = MIPSCacheMapping::Cached;
  B.Translation->PhysicalAddress = 0x23001;
  EXPECT_TRUE(Cache(21));
  EXPECT_TRUE(B.Line.Dirty);
  EXPECT_TRUE(B.Line.Valid);
  B.Translation->PhysicalAddress = 0x12001;
  B.Fail = true;
  EXPECT_FALSE(Cache(21));
  EXPECT_TRUE(B.Line.Locked);
  EXPECT_TRUE(B.Line.Dirty);
  B.Fail = false;
  EXPECT_TRUE(Cache(21));
  EXPECT_FALSE(B.Line.Valid);
  EXPECT_FALSE(B.Line.Dirty);
  EXPECT_FALSE(B.Line.Locked);
  EXPECT_EQ(B.Memory, B.Line.Data);
  EXPECT_EQ(B.LastVA, 0x80001233u);
  B.Translation->PhysicalAddress = 0x100023002;
  ASSERT_TRUE(Cache(20)); // I-cache fill
  EXPECT_EQ(B.Line.PhysicalBase, UINT64_C(0x100023000));
  EXPECT_EQ(B.Line.Data, (std::vector<uint8_t>{0x23, 0x24, 0x25, 0x26}));
  EXPECT_FALSE(B.Line.Locked);
  ASSERT_TRUE(Cache(28)); // A fetch-and-lock hit needs no memory read.
  EXPECT_EQ(B.Fills, 1u);
  EXPECT_TRUE(B.Line.Locked);
  B.Translation->PhysicalAddress = 0x12000;
  EXPECT_FALSE(Cache(29)); // The board selected a locked victim.
  EXPECT_EQ(B.Fills, 1u);
  B.Line = {0x12000, {5, 6, 7, 8}, true, true, false};
  B.Translation->PhysicalAddress = 0x100023002;
  B.Fail = true;
  EXPECT_FALSE(Cache(29));
  EXPECT_EQ(B.Line.PhysicalBase, 0x12000u);
  EXPECT_TRUE(B.Line.Dirty);
  B.Fail = false;
  ASSERT_TRUE(Cache(29));
  EXPECT_EQ(B.Memory, (std::vector<uint8_t>{5, 6, 7, 8}));
  EXPECT_TRUE(B.Line.Valid);
  EXPECT_TRUE(B.Line.Locked);
  EXPECT_FALSE(B.Line.Dirty);
}

TEST_P(MIPS16E2, MTExecutionBarrierDoesNotCertifyPeerInstructionSuspension) {
  BinaryImage I;
  I.Arch = GetParam();
  NdOpEmulator E(I);
  MIPSSystemContext C;
  C.ImplementedMT = C.CP0Usable = true;
  C.MVPControl = 1;
  C.VPEControl = 1u << 15;
  C.VPEConf0 = 2;
  const auto MT = [&](Intrinsic Id) {
    auto O = stateOp(Id, {});
    O.Output = NdVar::tmp(9, 4);
    return E.stepMIPSSystem(O, C);
  };
  EXPECT_EQ(C.peerIssueEnabled(true), true);
  ASSERT_TRUE(MT(Intrinsic::MIPSDisableMT));
  EXPECT_FALSE(C.peerIssueEnabled(true).has_value());
  ASSERT_TRUE(
      E.stepMIPSSystem(stateOp(Intrinsic::MIPSExecutionHazardBarrier, {}), C));
  EXPECT_FALSE(C.peerIssueEnabled(true).has_value());
  EXPECT_EQ(C.peerIssueEnabled(false), true);
  C.completeInstructionHazardBarrier();
  EXPECT_EQ(C.peerIssueEnabled(true), false);
  ASSERT_TRUE(MT(Intrinsic::MIPSDisableVPE));
  C.completeInstructionHazardBarrier();
  EXPECT_EQ(C.peerIssueEnabled(false), false);
  C.VPEConf0 = 0;
  ASSERT_TRUE(MT(Intrinsic::MIPSEnableVPE));
  EXPECT_EQ(C.peerIssueEnabled(false), false);
}
INSTANTIATE_TEST_SUITE_P(BothByteOrders, MIPS16E2,
                         testing::Values(Arch::MIPS32, Arch::MIPS32EL));
} // namespace

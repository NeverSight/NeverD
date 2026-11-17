//===- MIPSHistoricalScheduleTests.cpp - Retired instruction traces -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Manual-derived dependency traces, including noncontiguous control flow.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/low/MIPS/MIPSHistoricalSchedule.h"
#include "neverd/ir/low/NdOpEmulator.h"

namespace neverd::mips {
namespace {

HistoricalSchedule quiescent() {
  return *HistoricalSchedule::fromSnapshot(1, {});
}

bool instruction(HistoricalSchedule &Schedule, uint64_t PC, uint64_t Reads = 0,
                 uint64_t Delayed = 0, uint16_t Access = 0) {
  return Schedule.begin(PC, Reads, Delayed, Access) && Schedule.retire(PC);
}

TEST(MIPSHistoricalScheduling, RegisterDelayFollowsExecutedInstructions) {
  // LW v0,0(a0); B target; ADDU v1,v0,zero (slot): the branch separates
  // the load from its consumer even though branch and slot share one unit.
  auto S = quiescent();
  ASSERT_TRUE(instruction(S, 0x1000, 1u << 4, 1u << 2));
  EXPECT_FALSE(S.begin(0x1004, 1u << 2, 0, 0));
  ASSERT_TRUE(instruction(S, 0x1004));
  EXPECT_TRUE(instruction(S, 0x1008, 1u << 2));

  // A load in a call's slot remains pending at the callee's first instruction.
  ASSERT_TRUE(instruction(S, 0x2000));
  ASSERT_TRUE(instruction(S, 0x2004, 1u << 4, 1u << 2));
  EXPECT_FALSE(S.begin(0x8000, 1u << 2, 0, 0));
  ASSERT_TRUE(instruction(S, 0x8000, 1u << 5));
  EXPECT_TRUE(instruction(S, 0x8004, 1u << 2));
}

TEST(MIPSHistoricalScheduling, AnnulledSlotsCannotAgeHIOrLO) {
  for (const bool HI : {false, true}) {
    const uint16_t Read = HI ? ReadHI : ReadLO;
    const uint16_t Write = HI ? WriteHI : WriteLO;
    for (const bool ExecutesSlot : {false, true}) {
      auto S = quiescent();
      ASSERT_TRUE(instruction(S, 0x1000, 0, 0, Read));
      ASSERT_TRUE(instruction(S, 0x1004)); // Branch head.
      if (ExecutesSlot)
        ASSERT_TRUE(instruction(S, 0x1008));
      EXPECT_EQ(S.begin(0x9000, 0, 0, Write), ExecutesSlot);
      if (ExecutesSlot)
        ASSERT_TRUE(S.retire(0x9000));
    }
  }
}

TEST(MIPSHistoricalScheduling, HIAndLORestrictionsNameTheRespectiveRegister) {
  for (const bool HI : {false, true})
    for (unsigned Gap = 0; Gap != 3; ++Gap) {
      const uint16_t Read = HI ? ReadHI : ReadLO;
      const uint16_t Write = HI ? WriteHI : WriteLO;
      auto S = quiescent();
      ASSERT_TRUE(instruction(S, 0x1000, 0, 0, Read));
      for (unsigned I = 0; I != Gap; ++I)
        ASSERT_TRUE(instruction(S, 0x1004 + 4 * I));
      const auto Before = S.snapshot();
      EXPECT_EQ(S.begin(0x9000, 0, 0, Write), Gap == 2);
      if (Gap != 2) {
        EXPECT_EQ(S.snapshot(), Before);
        // MTHI after MFLO, or MTLO after MFHI, does not violate this rule.
        EXPECT_TRUE(instruction(S, 0x9000, 0, 0, HI ? WriteLO : WriteHI));
      }
    }
}

TEST(MIPSHistoricalScheduling, FloatingMovesAndEarlyConditionReadsDiffer) {
  for (unsigned FPR : {0u, 1u, 4u, 5u, 30u, 31u}) {
    auto S = quiescent();
    const auto Register = uint64_t{1} << (32 + FPR);
    ASSERT_TRUE(instruction(S, 0x1000, 1u << 4, Register)); // MTC1.
    EXPECT_FALSE(S.begin(0x1004, Register, 0, 0));
    ASSERT_TRUE(instruction(S, 0x1004));
    EXPECT_TRUE(instruction(S, 0x1008, Register));
  }
  for (unsigned Gap = 0; Gap != 3; ++Gap) {
    auto Control = quiescent();
    auto Compare = quiescent();
    ASSERT_TRUE(instruction(Control, 0x1000, 0, 0, WriteFCSR));
    ASSERT_TRUE(instruction(Compare, 0x1000, 0, 0, WriteFCC));
    for (unsigned I = 0; I != Gap; ++I) {
      ASSERT_TRUE(instruction(Control, 0x1004 + 4 * I));
      ASSERT_TRUE(instruction(Compare, 0x1004 + 4 * I));
    }
    EXPECT_EQ(Control.begin(0x8000, 0, 0, ReadFCC), Gap == 2);
    EXPECT_EQ(Compare.begin(0x8000, 0, 0, ReadFCC), Gap >= 1);
  }
}

TEST(MIPSHistoricalScheduling, FailedAttemptDoesNotCommitOrClearPriorHazards) {
  auto S = quiescent();
  ASSERT_TRUE(instruction(S, 0x1000, 0, 1u << 2, ReadLO));
  const auto Before = S.snapshot();
  ASSERT_TRUE(S.begin(0x1004, 1u << 4, 1u << 3, 0)); // Load faults.
  EXPECT_EQ(S.snapshot(), Before);
  EXPECT_FALSE(S.retire(0x1008));
  EXPECT_FALSE(S.begin(0x1008, 0, 0, 0));
  ASSERT_TRUE(S.abort());
  EXPECT_FALSE(S.abort());
  EXPECT_EQ(S.snapshot(), Before);
  EXPECT_FALSE(S.begin(0x1004, 1u << 2, 0, 0));
  EXPECT_FALSE(S.begin(0x1004, 0, 0, WriteLO));
  ASSERT_TRUE(instruction(S, 0x1004));
  EXPECT_TRUE(instruction(S, 0x1008, 1u << 2));
  EXPECT_TRUE(instruction(S, 0x100c, 0, 0, WriteLO));
}

TEST(MIPSHistoricalScheduling,
     SnapshotAndInstructionBoundariesRejectMalformedState) {
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(0, {}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(4, {}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {1, 0, 0, 0, 0}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {0, 2, 0, 0, 0}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {0, 0, 3, 0, 0}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {0, 0, 0, 3, 0}));
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {0, 0, 0, 0, 3}));
  auto S = quiescent();
  EXPECT_FALSE(S.retire(0x1000));
  EXPECT_FALSE(S.begin(0x1002, 0, 0, 0));
  EXPECT_FALSE(S.begin(0x1000, 0, 1, 0));
  EXPECT_FALSE(S.begin(0x1000, 0, 0, 0x100));
  EXPECT_EQ(S.snapshot(), HistoricalSchedulingState{});
  EXPECT_FALSE(S.hasActiveInstruction());
}

LowOp begin(uint64_t PC, uint64_t Reads = 0, uint64_t Delayed = 0,
            uint16_t Access = 0) {
  LowOp Op;
  Op.Addr = PC;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::MIPSHistoricalBegin), 4));
  Op.addInput(NdVar::cst(Reads, 8));
  Op.addInput(NdVar::cst(Delayed, 8));
  Op.addInput(NdVar::cst(Access, 2));
  return Op;
}

LowOp retire(uint64_t PC) {
  LowOp Op;
  Op.Addr = PC;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::MIPSHistoricalRetire), 4));
  return Op;
}

BinaryImage image(Arch Target, unsigned Level) {
  BinaryImage Img;
  Img.Arch = Target;
  Img.Bits = isMIPS64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Img.MIPS.emplace();
  Img.MIPS->ISARevision = 0;
  Img.MIPS->HistoricalISA = Level;
  Img.MIPS->ABI = isMIPS64(Target) ? MIPSABI::N64 : MIPSABI::O32;
  return Img;
}

TEST(MIPSHistoricalScheduling,
     BoundExecutionRetainsDependenciesAcrossConsumers) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Level = 1; Level <= 3; ++Level) {
      if (isMIPS64(Target) != (Level == 3))
        continue;
      auto Img = image(Target, Level);
      NdOpEmulator First(Img), Callee(Img);
      First.setStrictMode(true);
      Callee.setStrictMode(true);
      MIPSSystemContext Context;
      EXPECT_FALSE(First.stepMIPSSystem(begin(0x1000), Context));
      EXPECT_FALSE(First.step(begin(0x1000)));
      Context.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      // The transfer in a call's executed slot is pending at a different
      // function/consumer. No instruction entry or new emulator clears it.
      ASSERT_TRUE(First.stepMIPSSystem(begin(0x1004, 0, 1u << 2), Context));
      EXPECT_EQ(Context.Historical->snapshot().UnreadyRegisters, 0u);
      ASSERT_TRUE(First.stepMIPSSystem(retire(0x1004), Context));
      EXPECT_FALSE(Callee.stepMIPSSystem(begin(0x8000, 1u << 2), Context));
      ASSERT_TRUE(Callee.stepMIPSSystem(begin(0x8000, 1u << 4), Context));
      EXPECT_FALSE(Callee.stepMIPSSystem(retire(0x8004), Context));
      ASSERT_TRUE(Callee.stepMIPSSystem(retire(0x8000), Context));
      EXPECT_TRUE(Callee.stepMIPSSystem(begin(0x8004, 1u << 2), Context));
      EXPECT_TRUE(Callee.stepMIPSSystem(retire(0x8004), Context));
    }
}

TEST(MIPSHistoricalScheduling, UnknownAndMalformedTransitionsPreserveState) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(Target, 1);
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    MIPSSystemContext Context;
    Context.Historical = HistoricalSchedule::fromSnapshot(1, {0, 0, 0, 2, 0});
    const auto Before = Context.Historical->snapshot();
    auto Unknown = begin(0x1000);
    Unknown.Inputs[1] = NdVar::tmp(7, 8);
    EXPECT_FALSE(E.stepMIPSSystem(Unknown, Context));
    for (unsigned Field = 0; Field != 4; ++Field) {
      auto Malformed = begin(0x1000);
      Malformed.Inputs[Field].Size = 3;
      EXPECT_FALSE(E.stepMIPSSystem(Malformed, Context));
    }
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000, 0, 1), Context));
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1002), Context));
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000, 0, 0, 0x100), Context));
    EXPECT_FALSE(E.stepMIPSSystem(retire(0x1000), Context));
    EXPECT_EQ(Context.Historical->snapshot(), Before);
    EXPECT_FALSE(Context.Historical->hasActiveInstruction());
  }
}

TEST(MIPSHistoricalScheduling, ImageAndBoundProcessorMustNameTheSameProfile) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Level = 1; Level <= 3; ++Level) {
      auto Img = image(Target, Level);
      MIPSSystemContext Context;
      Context.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      NdOpEmulator E(Img);
      const bool WidthMatches = isMIPS64(Target) == (Level == 3);
      EXPECT_EQ(E.stepMIPSSystem(begin(0x1000), Context), WidthMatches);
      if (WidthMatches)
        ASSERT_TRUE(Context.Historical->abort());
      Context.Historical =
          HistoricalSchedule::fromSnapshot(Level == 1 ? 2 : 1, {});
      EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000), Context));
      Context.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      Img.MIPS->ISARevision = 2;
      EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000), Context));
      Img.MIPS->ISARevision = 0;
      Context.FloatingPointException = true;
      EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000), Context));
      Context.FloatingPointException = false;
      for (bool MIPSImageMetadata::*Feature :
           {&MIPSImageMetadata::HasMicroMIPS, &MIPSImageMetadata::HasMIPS16,
            &MIPSImageMetadata::HasMIPS16E2, &MIPSImageMetadata::HasMT,
            &MIPSImageMetadata::HasDSP, &MIPSImageMetadata::HasDSPR2,
            &MIPSImageMetadata::HasMSA}) {
        Img.MIPS.value().*Feature = true;
        EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000), Context));
        Img.MIPS.value().*Feature = false;
      }
      EXPECT_EQ(Context.Historical->snapshot(), HistoricalSchedulingState{});
    }
}

} // namespace
} // namespace neverd::mips

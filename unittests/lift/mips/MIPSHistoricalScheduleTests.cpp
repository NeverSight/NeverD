//===- MIPSHistoricalScheduleTests.cpp - Retired instruction traces -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Manual-derived dependency traces, including noncontiguous control flow.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/MIPS/MIPSHistoricalSchedule.h"
#include "neverd/ir/low/MIPS/MIPSScalarFPExecution.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <vector>

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/mips.h"
#include "unicorn/unicorn.h"

#include <memory>
#endif

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

LowOp begin(uint64_t PC, uint32_t Word = 0, bool Paired = false) {
  LowOp Op;
  Op.Addr = PC;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(
      NdVar::cst(unsigned(Intrinsic::MIPSHistoricalBegin), sizeof(Intrinsic)));
  Op.addInput(NdVar::cst(Word, 4));
  Op.addInput(NdVar::cst(Paired, 1));
  return Op;
}

LowOp retire(uint64_t PC) {
  LowOp Op;
  Op.Addr = PC;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(
      NdVar::cst(unsigned(Intrinsic::MIPSHistoricalRetire), sizeof(Intrinsic)));
  return Op;
}

BinaryImage image(Arch Target, unsigned Level) {
  BinaryImage Img;
  Img.Arch = Target;
  Img.Bits = isMIPSN64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Img.MIPS.emplace();
  Img.MIPS->ISARevision = 0;
  Img.MIPS->HistoricalISA = Level;
  Img.MIPS->ABI = isMIPSN64(Target)   ? MIPSABI::N64
                  : isMIPSN32(Target) ? MIPSABI::N32
                                      : MIPSABI::O32;
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
      Img.MIPS->FPABI = 1;
      const bool Paired = Level < 3;
      NdOpEmulator First(Img), Callee(Img);
      First.setStrictMode(true);
      Callee.setStrictMode(true);
      MIPSSystemContext Context;
      EXPECT_FALSE(First.stepMIPSSystem(begin(0x1000, 0, Paired), Context));
      // Constant recovery cannot approximate away a physical retirement
      // boundary, even when it permits unrelated opaque operations.
      for (bool Strict : {false, true}) {
        First.setStrictMode(Strict);
        EXPECT_FALSE(First.step(begin(0x1000, 0, Paired)));
        EXPECT_FALSE(First.step(retire(0x1000)));
      }
      Context.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      // The transfer in a call's executed slot is pending at a different
      // function/consumer. No instruction entry or new emulator clears it.
      ASSERT_TRUE(
          First.stepMIPSSystem(begin(0x1004, 0x44022000, Paired), Context));
      EXPECT_EQ(Context.Historical->snapshot().UnreadyRegisters, 0u);
      ASSERT_TRUE(First.stepMIPSSystem(retire(0x1004), Context));
      EXPECT_FALSE(
          Callee.stepMIPSSystem(begin(0x8000, 0x00401821, Paired), Context));
      ASSERT_TRUE(
          Callee.stepMIPSSystem(begin(0x8000, 0x00801821, Paired), Context));
      EXPECT_FALSE(Callee.stepMIPSSystem(retire(0x8004), Context));
      ASSERT_TRUE(Callee.stepMIPSSystem(retire(0x8000), Context));
      EXPECT_TRUE(
          Callee.stepMIPSSystem(begin(0x8004, 0x00401821, Paired), Context));
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
    Unknown.Inputs[1] = NdVar::tmp(7, 4);
    EXPECT_FALSE(E.stepMIPSSystem(Unknown, Context));
    for (unsigned Field = 0; Field != 3; ++Field) {
      auto Malformed = begin(0x1000);
      Malformed.Inputs[Field].Size = 3;
      EXPECT_FALSE(E.stepMIPSSystem(Malformed, Context));
    }
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000, 0, true), Context));
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1002), Context));
    EXPECT_FALSE(E.stepMIPSSystem(begin(0x1000, 0x78000000), Context));
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

std::vector<LowOp> lift(const BinaryImage &Img, uint64_t PC,
                        std::initializer_list<uint32_t> Words) {
  std::vector<uint8_t> Bytes;
  for (const auto Word : Words)
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(Word >> (8 * (archLittleEndian(Img.Arch) ? I : 3 - I)));
  Decoder D;
  EXPECT_TRUE(D.init(Img));
  DecodedExecutionUnit Unit;
  EXPECT_EQ(D.decodeUnitForLift(Bytes.data(), Bytes.size(), PC, Unit),
            Bytes.size());
  std::vector<LowOp> Ops;
  D.liftUnitToLow(Unit, Ops);
  return Ops;
}

bool execute(const std::vector<LowOp> &Ops, NdOpEmulator &E,
             MIPSSystemContext &Context) {
  for (const auto &Op : Ops) {
    if (Op.Opcode == NdOp::COND_BR) {
      const auto Taken = Op.Inputs[1].isConst()
                             ? std::optional<uint64_t>(Op.Inputs[1].Offset)
                             : E.getRegister(Op.Inputs[1].Offset);
      if (!Taken)
        return false;
      if (*Taken)
        return true;
      continue;
    }
    if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::CALL ||
        Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR ||
        Op.Opcode == NdOp::RETURN)
      return true;
    if (Op.Opcode == NdOp::INTRINSIC) {
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      if (isStatefulScalar(Id) || isScalarFPControl(Id) ||
          isScalarFPArithmetic(Id)) {
        if (!E.stepMIPSSystem(Op, Context))
          return false;
        continue;
      }
    }
    if (!E.step(Op))
      return false;
  }
  return true;
}

TEST(MIPSHistoricalScheduling, O32ReadsAndWritesFullMIPSIIIGPRs) {
  for (Arch A : {Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    auto Img = image(A, 3);
    Img.MIPS->FPABI = 1;
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(3, {});
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 0x100000001ULL);
    E.setRegister(mipsreg::A1, 0x123456787fffffffULL);
    const auto Add = lift(Img, 0x1000, {0x0085102d}); // DADDU.
    EXPECT_FALSE(execute(Add, E, C));
    EXPECT_FALSE(C.Historical->hasActiveInstruction());
    C.HistoricalO32DoublewordEnabled = true;
    ASSERT_TRUE(execute(Add, E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x1234567980000000ULL);
    ASSERT_TRUE(execute(lift(Img, 0x1004, {0x0002183f}), E, C)); // DSRA32.
    EXPECT_EQ(E.getRegister(mipsreg::V1), 0x12345679ULL);
    ASSERT_TRUE(execute(lift(Img, 0x1008, {0x00021000}), E, C)); // SLL v0,v0,0.
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0xffffffff80000000ULL);
  }
}

TEST(MIPSHistoricalScheduling,
     R4000DoublewordIssueUsesCurrentPrivilegeAndExtendedMode) {
  for (Arch A : {Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    auto Img = image(A, 3);
    const auto Slot = lift(Img, 0x1000, {0x10000002, 0x0085102d}); // DADDU.
    for (const auto &[Status, Enabled] : {std::pair{0u, true},
                                          {0x80u, true},
                                          {0x08u, false},
                                          {0x48u, true},
                                          {0x10u, false},
                                          {0x30u, true},
                                          {0x12u, true},
                                          {0x0cu, true}}) {
      SCOPED_TRACE(Status);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          3, {},
          HistoricalCP0{HistoricalProcessor::R4000, Status, 0, 0, {}, {}});
      ASSERT_TRUE(C.Historical);
      C.HistoricalO32DoublewordEnabled = !Enabled;
      NdOpEmulator E(Img);
      E.setRegister(mipsreg::A0, 0x100000001ULL);
      E.setRegister(mipsreg::A1, 0x200000008ULL);
      E.setRegister(mipsreg::V0, 0xfeed);
      EXPECT_EQ(execute(Slot, E, C), Enabled);
      const auto &System = *C.Historical->system();
      EXPECT_EQ(System.ExceptionVector.has_value(), !Enabled);
      if (Enabled) {
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0x300000009ULL);
      } else {
        EXPECT_EQ(System.Cause, 0x80000028u); // BD and reserved instruction.
        EXPECT_EQ(System.EPC, 0x1000u);
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0xfeedu);
      }
    }
  }
}

TEST(MIPSHistoricalScheduling, ErrorEPCAndStatusWritesReachTheSelectedReturn) {
  for (Arch A : {Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    auto Img = image(A, 3);
    for (bool WideMove : {false, true}) {
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          3, {},
          HistoricalCP0{HistoricalProcessor::R4000, 0, 0, 0x2000, {}, {}});
      NdOpEmulator E(Img);
      auto &S = *C.Historical;
      const auto Ref = WideMove ? 0x1012345678ULL : 0xffffffff82345678ULL;
      E.setRegister(mipsreg::A0, WideMove ? Ref : uint32_t(Ref));
      ASSERT_TRUE(execute(
          lift(Img, 0x1100, {WideMove ? 0x40a4f000u : 0x4084f000u}), E, C));
      ASSERT_EQ(S.system()->ErrorEPC, Ref);
      const auto Read = lift(Img, 0x1200, {0x4022f000}); // DMFC0 v0,ErrorEPC.
      EXPECT_FALSE(execute(Read, E, C));
      for (unsigned Gap = 0; Gap != 2; ++Gap)
        ASSERT_TRUE(execute(lift(Img, 0x1140 + 4 * Gap, {0}), E, C));
      ASSERT_TRUE(execute(Read, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), Ref);
      E.setRegister(mipsreg::A0, 4);
      ASSERT_TRUE(execute(lift(Img, 0x1210, {0x40846000}), E, C)); // Set ERL.
      for (unsigned Gap = 0; Gap != 3; ++Gap) {
        ASSERT_TRUE(S.beginPhysical(0x1220, 0x42000018, false));
        EXPECT_FALSE(S.stageExceptionReturn(0x1220, true));
        ASSERT_TRUE(S.abort());
        ASSERT_TRUE(execute(lift(Img, 0x1230 + 4 * Gap, {0}), E, C));
      }
      ASSERT_TRUE(S.beginPhysical(0x1240, 0x42000018, false));
      EXPECT_EQ(S.stageExceptionReturn(0x1240, true), Ref);
      ASSERT_TRUE(S.retire(0x1240));
      EXPECT_EQ(S.system()->Status, 0u);
      EXPECT_EQ(S.system()->EPC, 0x2000u);
    }
    for (unsigned Level : {1u, 2u}) {
      EXPECT_FALSE(historicalInstructionAvailable(0x4002f000, Level));
      EXPECT_FALSE(historicalInstructionAvailable(0x4084f000, Level));
    }
  }
}

TEST(MIPSHistoricalScheduling,
     UnalignedERETDestinationFaultsOnFetchAfterRestoringStatus) {
  for (Arch A : {Arch::MIPS64O32, Arch::MIPS64O32EL})
    for (unsigned Register : {14u, 30u})
      for (unsigned Offset : {1u, 2u}) {
        auto Img = image(A, 3);
        HistoricalCP0 Sys{HistoricalProcessor::R4000,
                          Register == 14 ? 2u : 4u,
                          0x80000000u,
                          0x2000,
                          {},
                          {}};
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(3, {}, Sys);
        NdOpEmulator E(Img);
        const auto Bad = 0xffffffffbfc00100ULL + Offset;
        E.setRegister(mipsreg::A0, Bad);
        ASSERT_TRUE(
            execute(lift(Img, 0x1000, {0x40a40000u | Register << 11}), E, C));
        for (unsigned Gap = 0; Gap != 2; ++Gap)
          ASSERT_TRUE(execute(lift(Img, 0x1004 + 4 * Gap, {0}), E, C));
        EXPECT_FALSE(execute(lift(Img, 0x1010, {0x42000018}), E, C));
        const auto &Result = *C.Historical->system();
        EXPECT_EQ(Result.Status, 2u);
        EXPECT_EQ(Result.Cause, 4u << 2);
        EXPECT_EQ(Result.EPC, Bad);
        EXPECT_EQ(Result.BadVAddr, Bad);
        EXPECT_EQ(Result.ExceptionVector, 0xffffffff80000180ULL);
        EXPECT_FALSE(C.Historical->hasActiveInstruction());
      }
}

TEST(MIPSHistoricalScheduling,
     ExceptionEPCDistinguishesOrdinaryAndDelaySlotEntry) {
  for (auto CPU :
       {HistoricalProcessor::R30xx, HistoricalProcessor::CW4011R3000,
        HistoricalProcessor::CW4011R4000, HistoricalProcessor::R4000})
    for (bool Little : {false, true})
      for (bool Slot : {false, true})
        for (bool Taken : {false, true}) {
          const unsigned Level = CPU == HistoricalProcessor::R30xx   ? 1
                                 : CPU == HistoricalProcessor::R4000 ? 3
                                                                     : 2;
          const auto A = Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                    : (Little ? Arch::MIPS32EL : Arch::MIPS32);
          auto Img = image(A, Level);
          MIPSSystemContext C;
          C.Historical = HistoricalSchedule::fromSnapshot(
              Level, {}, HistoricalCP0{CPU, 1, 0x500, 0x88, {}, {}});
          ASSERT_TRUE(C.Historical);
          NdOpEmulator E(Img);
          E.setStrictMode(true);
          E.setRegister(mipsreg::A0, Taken ? 0 : 7);
          const auto Ops = Slot ? lift(Img, 0x1000, {0x10800004, 0x0000000c})
                                : lift(Img, 0x1004, {0x0000000c});
          EXPECT_FALSE(execute(Ops, E, C));
          EXPECT_FALSE(C.Historical->hasActiveInstruction());
          const auto &System = *C.Historical->system();
          EXPECT_EQ(System.EPC, Slot ? 0x1000u : 0x1004u);
          EXPECT_EQ(System.Cause & 0x80000000u, Slot ? 0x80000000u : 0);
          EXPECT_EQ(System.Cause & 0x7c, 8u << 2); // SYSCALL.
          EXPECT_EQ(System.Cause & 0xff00, 0x500u);
          EXPECT_EQ(System.Status, System.usesModeStack() ? 4u : 3u);
          if (System.tracksBranchTaken())
            EXPECT_EQ(System.Cause >> 30 & 1, Slot && Taken);
          ASSERT_TRUE(System.ExceptionVector);
          EXPECT_FALSE(
              execute(lift(Img, *System.ExceptionVector, {0xd}), E, C));
          EXPECT_EQ(System.Cause & 0x7c, 9u << 2);
          if (System.usesModeStack())
            EXPECT_EQ(System.EPC, *System.ExceptionVector);
          else
            EXPECT_EQ(System.EPC, Slot ? 0x1000u : 0x1004u);
        }
}

TEST(MIPSHistoricalScheduling,
     CP0HazardsAgeOnlyAfterRetiredPhysicalInstructions) {
  for (bool Little : {false, true})
    for (auto CPU :
         {HistoricalProcessor::R30xx, HistoricalProcessor::CW4011R3000,
          HistoricalProcessor::CW4011R4000, HistoricalProcessor::R4000}) {
      const unsigned Level = CPU == HistoricalProcessor::R4000   ? 3
                             : CPU == HistoricalProcessor::R30xx ? 1
                                                                 : 2;
      auto Img = image(Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                  : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       Level);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          Level, {}, HistoricalCP0{CPU, 1, 0, 0x4000, {}, {}});
      NdOpEmulator E(Img);
      E.setRegister(mipsreg::A0, 0x100);
      ASSERT_TRUE(
          execute(lift(Img, 0x1000, {0x40846800}), E, C)); // MTC0 a0,Cause.
      const auto &CP0 = *C.Historical->system();
      EXPECT_EQ(CP0.Cause, 0x100u);
      const auto Read = lift(Img, 0x4000, {0x40026800}); // MFC0 v0,Cause.
      const unsigned Gap = Level == 1 ? 3 : Level == 2 ? 1 : 2;
      for (unsigned I = 0; I < Gap; ++I) {
        EXPECT_FALSE(execute(Read, E, C));
        EXPECT_FALSE(C.Historical->hasActiveInstruction());
        ASSERT_TRUE(execute(lift(Img, 0x2000 + 4 * I, {0}), E, C));
      }
      ASSERT_TRUE(execute(Read, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0x100u);
    }
}

TEST(MIPSHistoricalScheduling,
     ConditionalTrapsUseTheGPRWidthAndSignedImmediate) {
  for (auto CPU :
       {HistoricalProcessor::CW4011R3000, HistoricalProcessor::CW4011R4000,
        HistoricalProcessor::R4000})
    for (bool Little : {false, true}) {
      const bool Wide = CPU == HistoricalProcessor::R4000;
      const unsigned ISA = Wide ? 3 : 2;
      auto Img = image(Wide ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                            : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       ISA);
      for (unsigned Condition : {0u, 1u, 2u, 3u, 4u, 6u})
        for (bool Immediate : {false, true}) {
          const uint32_t Word =
              Immediate
                  ? (1u << 26) | (4u << 21) | ((8u + Condition) << 16) | 0xfffe
                  : (4u << 21) | (5u << 16) | (0x155u << 6) | (48u + Condition);
          ASSERT_FALSE(historicalInstructionAvailable(Word, 1));
          const auto Ops = lift(Img, 0x1080, {Word});
          for (int64_t Input : std::array<int64_t, 4>{-3, -2, 3, 0x1fffffffe}) {
            const int64_t Left = Wide ? Input : int32_t(Input);
            const auto Unsigned = [Wide](int64_t X) -> uint64_t {
              return Wide ? uint64_t(X) : uint32_t(X);
            };
            const bool Expected[] = {Left >= -2, Unsigned(Left) >= Unsigned(-2),
                                     Left < -2,  Unsigned(Left) < Unsigned(-2),
                                     Left == -2, false,
                                     Left != -2};
            SCOPED_TRACE(::testing::Message() << ISA << ' ' << Little << ' '
                                              << Word << ' ' << Input);
            MIPSSystemContext C;
            C.Historical = HistoricalSchedule::fromSnapshot(
                ISA, {}, HistoricalCP0{CPU, 0, 0, 0x1234, {}, {}});
            NdOpEmulator E(Img);
            E.setRegister(mipsreg::A0, Input);
            E.setRegister(mipsreg::A1, uint64_t(-2));
            const bool Trapped = Expected[Condition];
            EXPECT_EQ(execute(Ops, E, C), !Trapped);
            const auto &System = *C.Historical->system();
            EXPECT_EQ((System.Cause >> 2) & 31, Trapped ? 13u : 0u);
            EXPECT_EQ(System.Cause >> 31, 0u);
            EXPECT_EQ(System.EPC, Trapped ? 0x1080u : 0x1234u);
          }
        }
    }
}

TEST(MIPSHistoricalScheduling, ConditionalTrapInAnExecutedOrAnnulledSlot) {
  for (auto A : {Arch::MIPS64, Arch::MIPS64EL})
    for (bool Annul : {false, true})
      for (bool Taken : {false, true}) {
        auto Img = image(A, 3);
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(
            3, {},
            HistoricalCP0{HistoricalProcessor::R4000, 0, 0, 0x1234, {}, {}});
        NdOpEmulator E(Img);
        E.setRegister(mipsreg::A0, Taken ? 0 : 1);
        // BEQ or BEQL a0,zero followed by TEQ zero,zero.
        const uint32_t Branch = (Annul ? 0x50800002u : 0x10800002u);
        const bool Executed = !Annul || Taken;
        EXPECT_EQ(execute(lift(Img, 0x2000, {Branch, 0x34}), E, C), !Executed);
        const auto &System = *C.Historical->system();
        EXPECT_EQ((System.Cause >> 2) & 31, Executed ? 13u : 0u);
        EXPECT_EQ(System.Cause >> 31, unsigned(Executed));
        EXPECT_EQ(System.EPC, Executed ? 0x2000u : 0x1234u);
      }
}

TEST(MIPSHistoricalScheduling, AnnulledSyscallCannotChangeExceptionState) {
  for (bool Little : {false, true}) {
    auto Img = image(Little ? Arch::MIPS64EL : Arch::MIPS64, 3);
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {}, HistoricalCP0{HistoricalProcessor::R4000, 1, 0, 0x7774, {}, {}});
    NdOpEmulator E(Img);
    E.setRegister(mipsreg::A0, 5);
    ASSERT_TRUE(execute(lift(Img, 0x1000, {0x50800002, 0xc}), E, C)); // BEQL.
    EXPECT_EQ(C.Historical->system()->EPC, 0x7774u);
    EXPECT_FALSE(C.Historical->system()->ExceptionVector);
  }
}

TEST(MIPSHistoricalScheduling, ReturnsRestoreTheSelectedExceptionMode) {
  for (bool Little : {false, true})
    for (auto CPU :
         {HistoricalProcessor::R30xx, HistoricalProcessor::CW4011R3000,
          HistoricalProcessor::CW4011R4000, HistoricalProcessor::R4000}) {
      unsigned Level = CPU == HistoricalProcessor::R4000   ? 3u
                       : CPU == HistoricalProcessor::R30xx ? 1u
                                                           : 2u;
      auto Img = image(Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                  : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       Level);
      HistoricalCP0 System{CPU, 0, 0, 0x5000, {}, {}};
      System.Status = System.usesModeStack() ? 0x3c : 3;
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(Level, {}, System);
      NdOpEmulator E(Img);
      E.setRegister(mipsreg::r(26), 0x5000);
      if (System.usesModeStack()) {
        ASSERT_TRUE(execute(lift(Img, 0x1800, {0x03400008, 0x42000010}), E,
                            C)); // JR k0; RFE.
        EXPECT_EQ(C.Historical->system()->Status, 0x3fu);
      } else {
        ASSERT_TRUE(execute(lift(Img, 0x1800, {0x42000018}), E, C));
        EXPECT_EQ(C.Historical->system()->Status, 1u);
      }
    }
}

TEST(MIPSHistoricalScheduling,
     InterruptSamplingAndWritesHaveDistinctBoundaries) {
  for (bool Little : {false, true}) {
    auto Img = image(Little ? Arch::MIPS64EL : Arch::MIPS64, 3);
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {},
        HistoricalCP0{HistoricalProcessor::R4000, 0, 0x100, 0xdead, {}, {}});
    auto &S = *C.Historical;
    NdOpEmulator E(Img);
    E.setRegister(mipsreg::A0, 0x101);
    auto Set = lift(Img, 0x1000, {0x40846000}); // MTC0 a0,Status.
    ASSERT_TRUE(execute(Set, E, C));
    EXPECT_FALSE(S.system()->canTakeInterrupt());
    for (unsigned I = 0; I < 3; ++I) {
      ASSERT_TRUE(S.beginPhysical(0x2000, 0, false));
      EXPECT_FALSE(S.enterException(0x2000, 0));
      ASSERT_TRUE(S.abort());
      ASSERT_TRUE(execute(lift(Img, 0x4000 + I * 4, {0}), E, C));
    }
    EXPECT_TRUE(S.system()->canTakeInterrupt());
    ASSERT_TRUE(S.beginPhysical(0x2004, 0, false, true));
    ASSERT_TRUE(S.enterException(0x2004, 0));
    EXPECT_EQ(S.system()->EPC, 0x2000u);
    EXPECT_EQ(S.system()->Cause, 0x80000100u);
    EXPECT_FALSE(S.system()->canTakeInterrupt());
    EXPECT_EQ(S.system()->ExceptionVector, 0xffffffff80000180ULL);
    ASSERT_TRUE(S.beginPhysical(0x8000, 0x40847000, false));
    ASSERT_TRUE(S.stageCP0Write(0x8000, 14, 0x81234560, false));
    ASSERT_TRUE(S.abort());
    EXPECT_EQ(S.system()->EPC, 0x2000u);
  }
}

TEST(MIPSHistoricalScheduling, AddressAndArithmeticFaultsPrecedeWriteback) {
  for (bool Little : {false, true})
    for (unsigned Level : {1u, 2u, 3u}) {
      auto Img = image(Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                  : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       Level);
      const auto CPU = Level == 1   ? HistoricalProcessor::R30xx
                       : Level == 2 ? HistoricalProcessor::CW4011R4000
                                    : HistoricalProcessor::R4000;
      for (auto Word : {0x8c820000u, 0xac820000u, 0x20820001u, 0x00851022u}) {
        const bool Memory = (Word >> 26) >= 32;
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(
            Level, {}, HistoricalCP0{CPU, 0, 0, 0, {}, {}});
        NdOpEmulator E(Img);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, Memory ? 0x2001u : 0x7fffffffu);
        E.setRegister(mipsreg::A1, UINT64_MAX);
        E.setRegister(mipsreg::V0, 0x5678);
        ASSERT_FALSE(execute(lift(Img, 0x1000, {0x10000004, Word}), E, C));
        const auto &S = *C.Historical->system();
        ASSERT_TRUE(S.ExceptionVector);
        EXPECT_EQ(S.EPC, 0x1000u);
        EXPECT_TRUE(S.Cause >> 31);
        EXPECT_EQ((S.Cause >> 2) & 31,
                  Memory ? (Word >> 26 == 43 ? 5u : 4u) : 12u);
        if (Memory)
          EXPECT_EQ(S.BadVAddr, 0x2001u);
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0x5678u);
        EXPECT_FALSE(C.Historical->hasActiveInstruction());
      }
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          Level, {}, HistoricalCP0{CPU, 0x401, 0, 0, {}, {}});
      NdOpEmulator E(Img);
      E.setRegister(mipsreg::A0, 15);
      ASSERT_TRUE(execute(lift(Img, 0x3100, {0x20820001}), E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 16u);
      ASSERT_TRUE(C.Historical->setHardwareInterrupts(1));
      EXPECT_FALSE(execute(lift(Img, 0x3104, {0x20820002}), E, C));
      EXPECT_EQ(C.Historical->system()->EPC, 0x3104u);
      EXPECT_EQ(C.Historical->system()->Cause, 0x400u);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 16u);
    }
}

TEST(MIPSHistoricalScheduling,
     UnusableCoprocessorSuppliesCauseBeforeDestination) {
  for (bool Little : {false, true}) {
    auto Img = image(Little ? Arch::MIPS64EL : Arch::MIPS64, 3);
    // The image declares COP1 register transport. Live Status.CU1 separately
    // controls whether this particular execution may touch those registers.
    Img.MIPS->FPABI = 1;
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {}, HistoricalCP0{HistoricalProcessor::R4000, 0, 0, 0, {}, {}});
    NdOpEmulator E(Img);
    E.setRegister(mipsreg::V0, 0x67);
    EXPECT_FALSE(execute(lift(Img, 0x2000, {0x44020000}), E, C)); // MFC1 v0,f0.
    EXPECT_EQ(C.Historical->system()->Cause, (1u << 28) | (11u << 2));
    EXPECT_EQ(C.Historical->system()->EPC, 0x2000u);
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x67u);
  }
}

void data(BinaryImage &Img) {
  Segment S;
  S.VA = 0x2000;
  S.Data = archLittleEndian(Img.Arch)
               ? std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12, 1, 2, 3, 4}
               : std::vector<uint8_t>{0x12, 0x34, 0x56, 0x78, 1, 2, 3, 4};
  S.Size = S.FileSz = S.Data.size();
  S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(S));
}

TEST(MIPSHistoricalScheduling,
     PhysicalLoadsDistinguishIFromInterlockedIIAndIII) {
  for (bool Little : {false, true})
    for (unsigned Level : {1u, 2u, 3u}) {
      auto Img = image(Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                  : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       Level);
      data(Img);
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, 0x2000);
      E.setRegister(mipsreg::V1, 99);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x8c820000}), E, C)); // LW.
      EXPECT_EQ(execute(lift(Img, 0x8000, {0x00401821}), E, C), Level != 1);
      if (Level == 1) {
        EXPECT_EQ(E.getRegister(mipsreg::V1), 99u);
        // The branch head is a real separating instruction. The consumer is
        // its slot, not an operand of one indivisible branch/slot operation.
        ASSERT_TRUE(execute(lift(Img, 0x8000, {0x10000002, 0x00401821}), E, C));
      }
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0x12345678u);
      EXPECT_FALSE(C.Historical->hasActiveInstruction());
    }
}

TEST(MIPSHistoricalScheduling, ACallSlotLoadRemainsPendingAtTheCalleeEntry) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    data(Img);
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 0x2000);
    E.setRegister(mipsreg::V1, 99);
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(1, {});
    const auto Ops = lift(Img, 0x1000, {0x0c002000, 0x8c820000});
    ASSERT_TRUE(execute(Ops, E, C));
    EXPECT_EQ(E.getRegister(mipsreg::RA), 0x1008u);
    EXPECT_EQ(C.Historical->snapshot().UnreadyRegisters, 1u << 2);
    EXPECT_FALSE(execute(lift(Img, 0x8000, {0x00401821}), E, C));
    ASSERT_TRUE(execute(lift(Img, 0x8000, {0}), E, C));
    ASSERT_TRUE(execute(lift(Img, 0x8004, {0x00401821}), E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 0x12345678u);
  }
}

TEST(MIPSHistoricalScheduling, AnAnnulledPhysicalSlotDoesNotSeparateHILOUse) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool Taken : {false, true}) {
      const unsigned Level = isMIPS64(A) ? 3 : 2;
      auto Img = image(A, Level);
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::LO, 7);
      E.setRegister(mipsreg::A0, 17);
      E.setRegister(mipsreg::A1, Taken ? 17 : 19);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x00001012}), E, C));    // MFLO.
      ASSERT_TRUE(execute(lift(Img, 0x1004, {0x50850003, 0}), E, C)); // BEQL.
      EXPECT_EQ(C.Historical->snapshot().LOReadDelay, Taken ? 0 : 1);
      EXPECT_EQ(execute(lift(Img, 0x9000, {0x00800013}), E, C), Taken); // MTLO.
      EXPECT_EQ(E.getRegister(mipsreg::LO), Taken ? 17u : 7u);
    }
}

TEST(MIPSHistoricalScheduling, MergeForwardingDoesNotHideAnAliasingBaseRead) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool BaseAliases : {false, true}) {
      auto Img = image(A, 1);
      data(Img);
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, 0x2000);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(1, {});
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x8c820000}), E, C));
      EXPECT_EQ(
          execute(lift(Img, 0x1004, {BaseAliases ? 0x88420001u : 0x88820001u}),
                  E, C),
          !BaseAliases);
      if (!BaseAliases) {
        EXPECT_EQ(C.Historical->snapshot().UnreadyRegisters, 1u << 2);
        EXPECT_FALSE(execute(lift(Img, 0x1008, {0x00401821}), E, C));
        ASSERT_TRUE(execute(lift(Img, 0x1008, {0x98820002}), E, C)); // LWR.
      }
    }
}

TEST(MIPSHistoricalScheduling, FR0PairsIncludeTheOddWordButWordMovesDoNot) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Double : {false, true}) {
      auto Img = image(A, 1);
      Img.MIPS->FPABI = 1;
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, 42);
      E.setRegister(mipsreg::w(4), 17);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(1, {});
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x44842800}), E, C)); // MTC1 f5.
      const auto Move = Double ? 0x46202186u : 0x46002186u; // MOV.D/S f6,f4.
      EXPECT_EQ(execute(lift(Img, 0x1004, {Move}), E, C), !Double);
      if (Double) {
        ASSERT_TRUE(execute(lift(Img, 0x1004, {0}), E, C));
        ASSERT_TRUE(execute(lift(Img, 0x1008, {Move}), E, C));
        EXPECT_EQ(E.getRegister(mipsreg::w(7)), 42u);
      }
      EXPECT_EQ(E.getRegister(mipsreg::w(6)), 17u);
      EXPECT_THROW(lift(Img, 0x2000, {0x46002986}), UnliftedInstruction);
    }
}

TEST(MIPSHistoricalScheduling, AFirstHILOWriteInvalidatesTheCompanionProduct) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool HI : {false, true}) {
      const unsigned Level = isMIPS64(A) ? 3 : 1;
      auto Img = image(A, Level);
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, 3);
      E.setRegister(mipsreg::A1, 7);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x00850018}), E, C)); // MULT.
      ASSERT_TRUE(
          execute(lift(Img, 0x1004, {HI ? 0x00800011u : 0x00800013u}), E, C));
      EXPECT_FALSE(
          execute(lift(Img, 0x8000, {HI ? 0x00001012u : 0x00001010u}), E, C));
      ASSERT_TRUE(
          execute(lift(Img, 0x8000, {HI ? 0x00001010u : 0x00001012u}), E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 3u);
    }
}

TEST(MIPSHistoricalScheduling,
     AModernFPUEvaluatorDoesNotImplyAnEarlyProcessor) {
  class ModernFPU : public MIPSScalarFPExecutor {
  public:
    unsigned Calls = 0;
    bool supportsRevision(unsigned R) const override { return R == 2; }
    std::optional<MIPSScalarFPResult> control(unsigned, bool, uint32_t,
                                              uint32_t FCSR) override {
      ++Calls;
      return MIPSScalarFPResult{FCSR, 0};
    }
    std::optional<MIPSScalarFPResult>
    execute(Intrinsic, uint32_t FCSR, llvm::ArrayRef<uint64_t>) override {
      ++Calls;
      return MIPSScalarFPResult{FCSR, 0};
    }
  };
  for (bool Little : {false, true})
    for (unsigned Level : {1u, 2u, 3u}) {
      auto Img = image(Level == 3 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                                  : (Little ? Arch::MIPS32EL : Arch::MIPS32),
                       Level);
      Img.MIPS->FPABI = 1;
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 99);
      ModernFPU FPU;
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(Level, {});
      C.ScalarFPU = &FPU;
      C.FCSR = 0;
      EXPECT_FALSE(execute(lift(Img, 0x1000, {0x4442f800}), E, C));
      EXPECT_EQ(FPU.Calls, 0u);
      EXPECT_EQ(E.getRegister(mipsreg::V0), 99u);
      EXPECT_EQ(C.FCSR, 0u);
      ASSERT_TRUE(C.Historical->hasActiveInstruction());
      EXPECT_EQ(C.Historical->snapshot(), HistoricalSchedulingState{});
      ASSERT_TRUE(C.Historical->abort());
    }
}

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
TEST(MIPSHistoricalScheduling,
     ScheduledIntegerTracesMatchIndependentExecution) {
  // An interlocked executor can check values on valid schedules. It cannot
  // establish the early-processor behavior of an invalid schedule; the manual
  // dependency and annulment cases above deliberately test those separately.
  const uint32_t Words[] = {0x8c820000, 0,          0x00451821, 0x00650018,
                            0x00001012, 0,          0,          0x00a00011,
                            0x00001812, 0x03e00008, 0};
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (uint32_t Input :
         {0u, 1u, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x12345678u})
      for (uint32_t Addend : {0u, 1u, 7u, 31u, 512u}) {
        const unsigned Level = isMIPS64(A) ? 3 : 1;
        auto Img = image(A, Level);
        data(Img);
        for (unsigned I = 0; I != 4; ++I)
          Img.Segments[0].Data[I] =
              Input >> (8 * (archLittleEndian(A) ? I : 3 - I));
        NdOpEmulator E(Img);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, 0x2000);
        E.setRegister(mipsreg::A1, Addend);
        E.setRegister(mipsreg::RA, 0x1100);
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(Level, {});
        for (unsigned I = 0; I != 9; ++I)
          ASSERT_TRUE(execute(lift(Img, 0x1000 + 4 * I, {Words[I]}), E, C));
        ASSERT_TRUE(execute(lift(Img, 0x1024, {Words[9], Words[10]}), E, C));

        uc_engine *Raw = nullptr;
        const auto Mode = static_cast<uc_mode>(
            (isMIPS64(A) ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
            (archLittleEndian(A) ? UC_MODE_LITTLE_ENDIAN : UC_MODE_BIG_ENDIAN));
        ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
        const auto Close = [](uc_engine *U) { uc_close(U); };
        std::unique_ptr<uc_engine, decltype(Close)> U(Raw, Close);
        ASSERT_EQ(uc_mem_map(Raw, 0x1000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
        std::vector<uint8_t> Bytes;
        for (uint32_t Word : Words)
          for (unsigned I = 0; I != 4; ++I)
            Bytes.push_back(Word >> (8 * (archLittleEndian(A) ? I : 3 - I)));
        ASSERT_EQ(uc_mem_write(Raw, 0x1000, Bytes.data(), Bytes.size()),
                  UC_ERR_OK);
        ASSERT_EQ(uc_mem_write(Raw, 0x2000, Img.Segments[0].Data.data(), 8),
                  UC_ERR_OK);
        uint64_t Base = 0x2000, Argument = Addend, Return = 0x1100;
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &Base), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &Argument), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_31, &Return), UC_ERR_OK);
        ASSERT_EQ(uc_emu_start(Raw, 0x1000, 0x1100, 0, 128), UC_ERR_OK);
        for (const auto &[Original, Lifted] :
             {std::pair{UC_MIPS_REG_2, mipsreg::V0},
              std::pair{UC_MIPS_REG_3, mipsreg::V1},
              std::pair{UC_MIPS_REG_HI, mipsreg::HI},
              std::pair{UC_MIPS_REG_LO, mipsreg::LO}}) {
          uint64_t Value = 0;
          ASSERT_EQ(uc_reg_read(Raw, Original, &Value), UC_ERR_OK);
          const auto Actual = E.getRegister(Lifted);
          ASSERT_TRUE(Actual);
          EXPECT_EQ(isMIPS64(A) ? *Actual : uint64_t(uint32_t(*Actual)), Value);
        }
      }
}
#endif

} // namespace
} // namespace neverd::mips

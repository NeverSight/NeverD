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

#include <algorithm>
#include <array>
#include <span>
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
  HistoricalCP0 CP0{HistoricalProcessor::R30xx, 1, 0, 0, {}, {}};
  CP0.InterruptDelay = 3;
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {}, CP0));
  CP0.InterruptDelay = 1;
  CP0.R30xxInterrupts[0] = {2, 1};
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {}, CP0));
  CP0.InterruptDelay = 0;
  CP0.R30xxRecognized = false;
  EXPECT_FALSE(HistoricalSchedule::fromSnapshot(1, {}, CP0));
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

std::vector<uint8_t> encode(Arch Target,
                            std::initializer_list<uint32_t> Words) {
  std::vector<uint8_t> Bytes;
  for (const auto Word : Words)
    for (unsigned I = 0; I != 4; ++I)
      Bytes.push_back(Word >> (8 * (archLittleEndian(Target) ? I : 3 - I)));
  return Bytes;
}

void addCode(BinaryImage &Img, uint64_t PA,
             std::initializer_list<uint32_t> Words) {
  Segment Code;
  Code.VA = PA;
  Code.Data = encode(Img.Arch, Words);
  Code.Size = Code.FileSz = Code.Data.size();
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Segments.push_back(std::move(Code));
}

std::vector<LowOp> lift(const BinaryImage &Img, uint64_t PC,
                        std::initializer_list<uint32_t> Words) {
  const auto Bytes = encode(Img.Arch, Words);
  Decoder D;
  EXPECT_TRUE(D.init(Img));
  DecodedExecutionUnit Unit;
  EXPECT_EQ(D.decodeUnitForLift(Bytes.data(), Bytes.size(), PC, Unit),
            Bytes.size());
  std::vector<LowOp> Ops;
  D.liftUnitToLow(Unit, Ops);
  return Ops;
}

bool execute(std::span<const LowOp> Ops, NdOpEmulator &E,
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
  }
}

TEST(MIPSHistoricalScheduling, CW4011WordErrorEPCReachesTheSelectedReturn) {
  ASSERT_TRUE(historicalInstructionAvailable(0x4084f000, 2));
  ASSERT_TRUE(historicalInstructionAvailable(0x4002f000, 2));
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (auto CPU :
         {HistoricalProcessor::CW4011R3000, HistoricalProcessor::CW4011R4000}) {
      auto Img = image(A, 2);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          2, {}, HistoricalCP0{CPU, 4, 0, 0x2000, {}, {}});
      ASSERT_TRUE(C.Historical);
      auto &S = *C.Historical;
      EXPECT_FALSE(S.system()->read(30, false));
      EXPECT_FALSE(S.system()->canWrite(30, 0x80000102, false));
      EXPECT_FALSE(S.system()->canWrite(30, 0x80000100, true));

      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 0x1234);
      E.setRegister(mipsreg::A0, 0x80000100);
      ASSERT_TRUE(execute(lift(Img, 0x1000, {0x4084f000}), E, C));
      ASSERT_EQ(S.system()->ErrorEPC, 0x80000100u);
      const auto Read = lift(Img, 0x1008, {0x4002f000});
      EXPECT_FALSE(execute(Read, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0x1234u);
      ASSERT_FALSE(S.hasActiveInstruction());
      ASSERT_TRUE(execute(lift(Img, 0x1004, {0}), E, C));
      ASSERT_TRUE(execute(Read, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0x80000100u);

      const bool ERET = CPU == HistoricalProcessor::CW4011R4000;
      ASSERT_TRUE(
          S.beginPhysical(0x100c, ERET ? 0x42000018 : 0x42000010, false));
      EXPECT_EQ(S.stageExceptionReturn(0x100c, ERET),
                ERET ? 0x80000100u : 0x2000u);
      ASSERT_TRUE(S.retire(0x100c));
      EXPECT_EQ(S.system()->Status, ERET ? 0u : 1u);
      EXPECT_EQ(S.system()->EPC, 0x2000u);
    }
  for (uint32_t Word : {0x4002f000u, 0x4084f000u}) {
    EXPECT_FALSE(historicalInstructionAvailable(Word, 1));
    EXPECT_FALSE(historicalInstructionAvailable(Word | 1, 2));
  }
  for (uint32_t Word : {0x4022f000u, 0x40a4f000u})
    EXPECT_FALSE(historicalInstructionAvailable(Word, 2));
}

TEST(MIPSHistoricalScheduling, CW4011SnapshotsUseItsPhysicalRegisterWidths) {
  for (auto CPU :
       {HistoricalProcessor::CW4011R3000, HistoricalProcessor::CW4011R4000}) {
    HistoricalCP0 System{CPU, 0, 0, 0x2000, {}, {}};
    for (uint32_t Mode : {0u, 8u, 16u, 24u}) {
      System.Status = Mode;
      EXPECT_EQ(HistoricalSchedule::fromSnapshot(2, {}, System).has_value(),
                CPU == HistoricalProcessor::CW4011R3000 || Mode == 0 ||
                    Mode == 16);
    }
    System.Status = 0;
    for (uint64_t Address : {0x100000000ULL, 3ULL}) {
      System.ErrorEPC = Address;
      EXPECT_FALSE(HistoricalSchedule::fromSnapshot(2, {}, System));
    }
    System.ErrorEPC = 0xfffffffc;
    EXPECT_TRUE(HistoricalSchedule::fromSnapshot(2, {}, System));
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

TEST(MIPSHistoricalScheduling, R30xxMixedInterruptRequiresAnIssueSample) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    const auto Set = lift(Img, 0x1000, {0x40846000}); // MTC0 a0,Status.
    const auto Add = lift(Img, 0x1004, {0x24021234}); // ADDIU v0,zero,0x1234.
    for (auto Old : {1u, 0x400u}) {
      for (bool Taken : {false, true}) {
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(
            1, {},
            HistoricalCP0{HistoricalProcessor::R30xx, Old, 0x400, 0, {}, {}});
        NdOpEmulator E(Img);
        auto &S = *C.Historical;
        E.setRegister(mipsreg::A0, Old ^ 0x401);
        E.setRegister(mipsreg::V0, 0xa5a5);
        ASSERT_TRUE(execute(Set, E, C));
        EXPECT_EQ(S.system()->interruptRecognition(), std::nullopt);
        EXPECT_FALSE(execute(Add, E, C));
        EXPECT_FALSE(S.hasActiveInstruction());
        EXPECT_FALSE(S.system()->ExceptionVector);
        EXPECT_EQ(S.system()->InterruptDelay, 2);
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0xa5a5u);

        ASSERT_TRUE(S.observeInterruptRecognition(false));
        ASSERT_TRUE(S.beginPhysical(0x1004, 0x24021234, false));
        EXPECT_FALSE(S.observeInterruptRecognition(true));
        ASSERT_TRUE(S.abort());
        ASSERT_TRUE(S.setHardwareInterrupts(1)); // Same input, same sample.
        EXPECT_EQ(S.system()->interruptRecognition(), false);
        EXPECT_FALSE(S.observeInterruptRecognition(true));
        ASSERT_TRUE(S.setHardwareInterrupts(0));
        EXPECT_EQ(S.system()->interruptRecognition(), false);
        ASSERT_TRUE(S.setHardwareInterrupts(1));
        ASSERT_TRUE(S.observeInterruptRecognition(Taken));
        EXPECT_EQ(execute(Add, E, C), !Taken);
        EXPECT_EQ(E.getRegister(mipsreg::V0), Taken ? 0xa5a5u : 0x1234u);
        if (Taken) {
          EXPECT_EQ(S.system()->ExceptionVector, 0x80000080u);
          EXPECT_EQ(S.system()->EPC, 0x1004u);
          EXPECT_EQ(S.system()->Cause, 0x400u);
          EXPECT_EQ(S.system()->interruptRecognition(), false);
        } else {
          EXPECT_FALSE(S.system()->ExceptionVector);
          EXPECT_EQ(S.system()->interruptRecognition(), std::nullopt);
          ASSERT_TRUE(S.observeInterruptRecognition(false));
          ASSERT_TRUE(execute(lift(Img, 0x1008, {0}), E, C));
          EXPECT_EQ(S.system()->interruptRecognition(), false);
        }
      }
    }
  }
}

// SCPH-5501 irqsio/14IEX, card-b, card-g and card-g3. The console
// counts instructions after MTC0 and samples SR in the handler. The probe
// PCs and counter instruction below are representative.
TEST(MIPSHistoricalScheduling, SCPH5501PendingIP2OnIEEnable) {
  const auto Img = image(Arch::MIPS32EL, 1);
  MIPSSystemContext Context;
  Context.Historical = HistoricalSchedule::fromSnapshot(
      1, {},
      HistoricalCP0{HistoricalProcessor::R30xx, 0x40000400, 0, 0, {}, {}});
  ASSERT_TRUE(Context.Historical);
  ASSERT_TRUE(Context.Historical->setHardwareInterrupts(1));
  NdOpEmulator Emulator(Img);
  Emulator.setRegister(mipsreg::A0, 0x40000401);
  Emulator.setRegister(mipsreg::V0, 0);
  const uint32_t PC = 0x80010000;
  ASSERT_TRUE(execute(lift(Img, PC, {0x40846000}), Emulator, Context));
  for (uint32_t I = 1; I <= 2; ++I) {
    ASSERT_TRUE(
        execute(lift(Img, PC + I * 4, {0x24420001}), Emulator, Context));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), I);
  }
  EXPECT_FALSE(execute(lift(Img, PC + 12, {0x24420001}), Emulator, Context));
  const auto &CPU = *Context.Historical->system();
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 2u);
  EXPECT_EQ(CPU.ExceptionVector, 0x80000080u);
  EXPECT_EQ(CPU.EPC, PC + 12);
  EXPECT_EQ(CPU.Status, 0x40000404u);
}

// SCPH-5501 irqsio 14IEX: the timer interrupts a three-word loop. The
// three distinct (EPC word relative to the mode write, Cause.BD) samples
// repeat as (2,0), (2,1), (1,0). Replay those issue boundaries with a
// representative loop; the console did not probe the electrical IP2 edge.
TEST(MIPSHistoricalScheduling, SCPH5501TimerIRQRecordsTheInterruptedLoopWord) {
  constexpr uint32_t ModePC = 0x80010000;
  constexpr std::array<unsigned, 3> SavedWords{2, 2, 1};
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    const auto Body = lift(Img, ModePC + 4, {0x24630002}); // ADDIU v1,v1,2.
    const auto Branch = lift(Img, ModePC + 8, {0x1000fffe, 0x24420001});
    const auto Slot =
        std::find_if(Branch.begin(), Branch.end(), [](const LowOp &Op) {
          return Op.Addr == ModePC + 12 && Op.Opcode == NdOp::INTRINSIC &&
                 Op.Inputs[0].Offset ==
                     unsigned(Intrinsic::MIPSHistoricalBegin);
        });
    ASSERT_NE(Slot, Branch.end());
    const std::array<std::span<const LowOp>, 3> Phases{
        std::span(Branch.begin(), Slot), std::span(Slot, Branch.end()), Body};
    for (unsigned Phase = 0; Phase != Phases.size(); ++Phase) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << " phase=" << Phase);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          1, {},
          HistoricalCP0{HistoricalProcessor::R30xx, 0x40000401, 0, 0, {}, {}});
      ASSERT_TRUE(C.Historical);
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 0);
      E.setRegister(mipsreg::V1, 0);
      for (unsigned I = 0; I != Phase; ++I)
        ASSERT_TRUE(execute(Phases[I], E, C));
      ASSERT_TRUE(C.Historical->setHardwareInterrupts(1));
      EXPECT_FALSE(execute(Phases[Phase], E, C));
      const auto &CPU = *C.Historical->system();
      EXPECT_EQ(CPU.EPC, ModePC + 4 * SavedWords[Phase]);
      EXPECT_EQ(CPU.Cause, Phase == 1 ? 0x80000400u : 0x400u);
      EXPECT_EQ(CPU.Status, 0x40000404u);
      EXPECT_EQ(CPU.ExceptionVector, 0x80000080u);
      EXPECT_EQ(E.getRegister(mipsreg::V0), Phase == 2 ? 1u : 0u);
      EXPECT_EQ(E.getRegister(mipsreg::V1), 0u);
    }
  }
}

TEST(MIPSHistoricalScheduling, R30xxInterruptWindowIsSpecificToTheSource) {
  struct Transition {
    uint32_t Old, New;
    std::optional<bool> FirstTwo;
    bool Settled;
  };
  const Transition Cases[] = {
      {0, 0x401, false, true},     // Enable both gates.
      {0x400, 0x401, false, true}, // Only IEc changes.
      {1, 0x401, false, true},     // Only IM changes.
      {0x401, 0xc01, true, true},  // A different line changes.
      {0x401, 0x400, {}, false},   // Disable the global gate.
      {0x401, 1, {}, false},       // Mask the active line.
      {0x401, 0, {}, false},       // Close both gates.
      {0, 0x801, false, false},    // Only the masked input is pending.
  };
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    const auto Set = lift(Img, 0x1000, {0x40846000});
    const auto Nop = lift(Img, 0x2000, {0});
    for (const auto &T : Cases) {
      SCOPED_TRACE(T.Old);
      SCOPED_TRACE(T.New);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          1, {},
          HistoricalCP0{HistoricalProcessor::R30xx, T.Old, 0, 0, {}, {}});
      NdOpEmulator E(Img);
      E.setRegister(mipsreg::A0, T.New);
      ASSERT_TRUE(execute(Set, E, C));
      auto &S = *C.Historical;
      for (unsigned I = 0; I < 2; ++I) {
        ASSERT_TRUE(S.setHardwareInterrupts(1));
        EXPECT_EQ(S.system()->interruptRecognition(), T.FirstTwo);
        EXPECT_EQ(S.observeInterruptRecognition(false),
                  !T.FirstTwo.has_value());
        ASSERT_TRUE(S.setHardwareInterrupts(0));
        ASSERT_TRUE(execute(Nop, E, C));
      }
      ASSERT_TRUE(S.setHardwareInterrupts(1));
      EXPECT_EQ(S.system()->interruptRecognition(), T.Settled);
      EXPECT_FALSE(S.observeInterruptRecognition(!T.Settled));
    }
  }
}

TEST(MIPSHistoricalScheduling, R30xxOverlappingWritesAgeEachInterruptField) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(
        1, {}, HistoricalCP0{HistoricalProcessor::R30xx, 0x101, 0, 0, {}, {}});
    auto &S = *C.Historical;
    NdOpEmulator E(Img);
    E.setRegister(mipsreg::A0, 0x100);
    ASSERT_TRUE(execute(lift(Img, 0x1000, {0x40846800}), E, C)); // Cause.IP0.
    E.setRegister(mipsreg::A0, 0x501);
    ASSERT_TRUE(execute(lift(Img, 0x1004, {0x40846000}), E, C)); // Unmask HW0.
    EXPECT_EQ(S.system()->interruptRecognition(), false);
    ASSERT_TRUE(execute(lift(Img, 0x1008, {0}), E, C));
    EXPECT_EQ(S.system()->interruptRecognition(), true); // IP0 has propagated.
    EXPECT_FALSE(execute(lift(Img, 0x100c, {0}), E, C));
    EXPECT_EQ(S.system()->EPC, 0x100cu);
    EXPECT_EQ(S.system()->Cause, 0x100u);
    EXPECT_EQ(S.system()->interruptRecognition(),
              false); // Handler IEc is clear.
  }
}

TEST(MIPSHistoricalScheduling, R30xxPendingDisableCanInterruptABranchSlot) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    auto Img = image(A, 1);
    MIPSSystemContext C;
    C.Historical = HistoricalSchedule::fromSnapshot(
        1, {}, HistoricalCP0{HistoricalProcessor::R30xx, 0x401, 0, 0, {}, {}});
    auto &S = *C.Historical;
    NdOpEmulator E(Img);
    E.setRegister(mipsreg::A0, 0x400);
    E.setRegister(mipsreg::V0, 99);
    ASSERT_TRUE(execute(lift(Img, 0x1000, {0x40846000}), E, C));
    // The board raises IP2 after BEQ retires, just before its ADDIU slot.
    const auto Branch = lift(Img, 0x1004, {0x10000002, 0x24021234});
    bool Delivered = false;
    for (const auto &Op : Branch) {
      if (Op.Addr == 0x1008 && Op.Opcode == NdOp::INTRINSIC &&
          Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSHistoricalBegin)) {
        ASSERT_TRUE(S.setHardwareInterrupts(1));
        ASSERT_TRUE(S.observeInterruptRecognition(true));
        EXPECT_FALSE(E.stepMIPSSystem(Op, C));
        Delivered = true;
        break;
      }
      ASSERT_TRUE(execute(std::span(&Op, 1), E, C));
    }
    EXPECT_TRUE(Delivered);
    EXPECT_EQ(S.system()->EPC, 0x1004u);
    EXPECT_EQ(S.system()->Cause, 0x80000400u);
    EXPECT_EQ(E.getRegister(mipsreg::V0), 99u);
    EXPECT_FALSE(S.hasActiveInstruction());
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

TEST(MIPSHistoricalScheduling, R4000DataFaultsPrecedeInterruptAtSameIssue) {
  constexpr uint64_t BranchPC = UINT64_C(0xffffffff80001000);
  constexpr uint32_t Page = 0x40000, Frame = 0x9000;
  constexpr uint32_t Branch = 0x10000001, Load = 0x8c820000, Store = 0xac820000;
  constexpr uint32_t Original = 0x13572468, Marker = 0x56781234;
  struct Case {
    uint32_t Word, Address, Flags, Code, VectorOffset;
  };
  for (Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    for (auto T :
         {Case{Load, Page + 0x2000, 6, 2, 0}, // Refill.
          Case{Store, Page + 0x2000, 6, 3, 0},
          Case{Load, Page + 0x1000, 6, 2, 0x180}, // Invalid odd page.
          Case{Store, Page + 0x1000, 6, 3, 0x180},
          Case{Store, Page, 2, 1, 0x180}, // Clean even page.
          Case{Load, Page + 1, 6, 4, 0x180}, Case{Store, Page + 1, 2, 5, 0x180},
          Case{Load, Page, 6, 0, 0x180}, // Valid access loses to IRQ.
          Case{Store, Page, 6, 0, 0x180}}) {
      auto Img = image(A, 3);
      addCode(Img, 0x1000, {Branch, T.Word, Load});
      addCode(Img, Frame, {Original});
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      std::array<HistoricalTLBEntry, 48> Entries{};
      Entries[0] = {Page, (Frame >> 6) | T.Flags, 0, 0};
      const HistoricalTLB MMU{Entries, 0};
      for (bool InSlot : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << " word=" << T.Word << " va=" << T.Address
                     << " code=" << T.Code << " slot=" << InSlot);
        MIPSSystemContext C;
        C.HistoricalMMU = &MMU;
        C.Historical = HistoricalSchedule::fromSnapshot(
            3, {},
            HistoricalCP0{
                HistoricalProcessor::R4000, 0x401, 0, 0, 0xfeed0000, {}});
        NdOpEmulator E(Img);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, T.Address);
        E.setRegister(mipsreg::V0, Marker);
        const auto Ops = InSlot ? lift(Img, BranchPC, {Branch, T.Word})
                                : lift(Img, BranchPC + 4, {T.Word});
        const auto Issue =
            std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
              return Op.Addr == BranchPC + 4 && Op.Opcode == NdOp::INTRINSIC &&
                     Op.Inputs[0].Offset ==
                         unsigned(Intrinsic::MIPSHistoricalBegin);
            });
        ASSERT_NE(Issue, Ops.end());
        ASSERT_TRUE(execute(std::span(Ops.begin(), Issue), E, C));
        ASSERT_TRUE(C.Historical->setHardwareInterrupts(1, BranchPC + 4));
        EXPECT_FALSE(execute(std::span(Issue, Ops.end()), E, C));
        const auto &CPU = *C.Historical->system();
        EXPECT_EQ(CPU.Cause, (InSlot ? 0x80000400u : 0x400u) | T.Code << 2);
        EXPECT_EQ(CPU.EPC, InSlot ? BranchPC : BranchPC + 4);
        EXPECT_EQ(CPU.BadVAddr, T.Code ? T.Address : 0xfeed0000u);
        EXPECT_EQ(CPU.ExceptionVector,
                  UINT64_C(0xffffffff80000000) + T.VectorOffset);
        EXPECT_FALSE(C.Historical->hasActiveInstruction());
        EXPECT_EQ(E.getRegister(mipsreg::V0), Marker);
        E.setRegister(mipsreg::A0, Page);
        ASSERT_TRUE(execute(lift(Img, BranchPC + 8, {Load}), E, C));
        EXPECT_EQ(E.getRegister(mipsreg::V0), Original);
      }
    }
  }
}

TEST(MIPSHistoricalScheduling, R4000SynchronousExceptionsPrecedeInterrupt) {
  struct Case {
    uint32_t Word, Status, Code;
  };
  for (Arch A : {Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    auto Img = image(A, 3);
    Img.MIPS->FPABI = 1;
    for (auto T : {Case{0x0000000c, 0x401, 8},    // SYSCALL
                   Case{0x0003454d, 0x401, 9},    // BREAK with a code
                   Case{0x20820001, 0x401, 12},   // ADDI overflows
                   Case{0x20820000, 0x401, 0},    // ADDI succeeds
                   Case{0x00850034, 0x401, 13},   // TEQ true
                   Case{0x00850036, 0x401, 0},    // TNE false
                   Case{0x44020000, 0x401, 11},   // MFC1, CU1 disabled
                   Case{0x0085102d, 0x411, 10},   // DADDU in 32-bit user mode
                   Case{0x0085102d, 0x431, 0},    // DADDU, UX enabled
                   Case{0x24020001, 0x401, 0}}) { // ADDIU
      SCOPED_TRACE(::testing::Message()
                   << unsigned(A) << " word=" << T.Word << " code=" << T.Code);
      MIPSSystemContext C;
      C.Historical = HistoricalSchedule::fromSnapshot(
          3, {},
          HistoricalCP0{HistoricalProcessor::R4000, T.Status, 0, 0, {}, {}});
      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::V0, 0xabcd);
      E.setRegister(mipsreg::A0, 0x7fffffff);
      E.setRegister(mipsreg::A1, 0x7fffffff);
      ASSERT_TRUE(C.Historical->setHardwareInterrupts(1, 0x2000));
      EXPECT_FALSE(execute(lift(Img, 0x2000, {T.Word}), E, C));
      const auto &CPU = *C.Historical->system();
      EXPECT_EQ(CPU.Cause,
                0x400u | (T.Code << 2) | (T.Code == 11 ? 1u << 28 : 0u));
      EXPECT_EQ(CPU.EPC, 0x2000u);
      EXPECT_EQ(CPU.ExceptionVector, UINT64_C(0xffffffff80000180));
      EXPECT_FALSE(C.Historical->hasActiveInstruction());
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0xabcdu);
    }
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

// These board consoles report fault state, not the prior TLB contents. A
// complete constructed table gives the observed exception a reproducible cause.
// Only the DECstation trace also identifies the exact faulting instruction.
TEST(MIPSHistoricalScheduling, GuestTLBExceptionsMatchHardwareConsoleFields) {
  struct Capture {
    Arch Target;
    unsigned Level;
    uint64_t PC;
    uint32_t Address, Word, BeforeStatus, CauseIP, Status, Code;
  };
  for (const auto &T :
       {Capture{Arch::MIPS32EL, 1, 0x80125a3c, 0xff00, 0x8c830000, 0x20080005,
                0, 0x20080014, 2}, // DECstation 5000/133.
        Capture{Arch::MIPS32, 1, 0x803742b4, 0, 0x8c830000, 0x8001, 0, 0x8004,
                2}, // SGI IP12, read.
        Capture{Arch::MIPS64, 3, UINT64_C(0xffffffffbfc394ec), 0, 0xac830000,
                0x20004001, 0x8000, 0x20004003, 3}}) { // SGI IP20, write.
    auto Img = image(T.Target, T.Level);
    Segment Code;
    Code.VA = T.PC & 0x1fffffffu;
    for (unsigned I = 0; I < 4; ++I)
      Code.Data.push_back(T.Word >>
                          (8 * (archLittleEndian(T.Target) ? I : 3 - I)));
    Code.Size = Code.FileSz = Code.Data.size();
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Segments.push_back(std::move(Code));
    std::vector<HistoricalTLBEntry> Entries(T.Level == 1 ? 64 : 48);
    for (unsigned I = 0; I < Entries.size(); ++I)
      Entries[I].Hi = 0x1000000 + (I << 13);
    const HistoricalTLB MMU{Entries, 7};
    MIPSSystemContext C;
    C.HistoricalMMU = &MMU;
    const auto Model =
        T.Level == 1 ? HistoricalProcessor::R30xx : HistoricalProcessor::R4000;
    C.Historical = HistoricalSchedule::fromSnapshot(
        T.Level, {}, HistoricalCP0{Model, T.BeforeStatus, 0, 0x88, {}, {}});
    ASSERT_TRUE(C.Historical);
    ASSERT_TRUE(C.Historical->setHardwareInterrupts(T.CauseIP >> 10, T.PC));
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, T.Address);
    E.setRegister(mipsreg::V1, 0x78);
    EXPECT_FALSE(execute(lift(Img, T.PC, {T.Word}), E, C));
    ASSERT_FALSE(C.Historical->hasActiveInstruction());
    const auto &CPU = *C.Historical->system();
    EXPECT_EQ(CPU.EPC, T.PC);
    EXPECT_EQ(CPU.Status, T.Status);
    EXPECT_EQ((CPU.Cause >> 2) & 31, T.Code);
    EXPECT_EQ(CPU.Cause & 0x8000ff00u, T.CauseIP);
    EXPECT_EQ(CPU.BadVAddr, T.Address);
    EXPECT_EQ(CPU.ExceptionVector, T.Level == 1 ? UINT64_C(0x80000000)
                                                : UINT64_C(0xffffffff80000000));
    EXPECT_EQ(E.getRegister(mipsreg::V1), 0x78u);
  }
}

TEST(MIPSHistoricalScheduling,
     TLBMatchPermissionsAndPairedPagesAreIndependent) {
  for (bool R3000 : {false, true}) {
    HistoricalCP0 CPU{R3000 ? HistoricalProcessor::R30xx
                            : HistoricalProcessor::R4000,
                      0,
                      0,
                      0,
                      {},
                      {}};
    std::vector<HistoricalTLBEntry> Entries(R3000 ? 64 : 48);
    for (unsigned I = 0; I < Entries.size(); ++I)
      Entries[I].Hi = 0x1000000 + (I << 13);
    auto &E = Entries.front();
    E = R3000 ? HistoricalTLBEntry{0x412c0, 0x54320200, 0, 0}
              : HistoricalTLBEntry{0x4000b, 0x100002, 0x200006, 0x6000};
    HistoricalTLB Table{Entries, 11};
    const auto Check = [&](uint64_t VA, HistoricalMemoryAccess A,
                           HistoricalAddressResult Kind, uint64_t PA = 0) {
      const auto T = Table.translate(CPU, VA, A);
      ASSERT_TRUE(T);
      EXPECT_EQ(T->Result, Kind);
      EXPECT_EQ(T->Physical, PA);
    };
    Check(0x41180, HistoricalMemoryAccess::Load,
          HistoricalAddressResult::Mapped, R3000 ? 0x54320180u : 0x4001180u);
    Check(0x41180, HistoricalMemoryAccess::Store,
          HistoricalAddressResult::Modified);
    if (!R3000)
      Check(0x44020, HistoricalMemoryAccess::Store,
            HistoricalAddressResult::Mapped, 0x8000020u);
    Table.ASID = 3;
    Check(0x41180, HistoricalMemoryAccess::Load,
          HistoricalAddressResult::Refill);
    if (R3000)
      E.Lo0 = 0x100;
    else
      E.Lo0 = E.Lo1 = 1;
    Check(0x41180, HistoricalMemoryAccess::Fetch,
          HistoricalAddressResult::Invalid);
    Entries[1] = E;
    EXPECT_FALSE(Table.translate(CPU, 0x41180, HistoricalMemoryAccess::Load));
    Table.Entries = Table.Entries.first(3);
    EXPECT_FALSE(Table.translate(CPU, 0x41180, HistoricalMemoryAccess::Load));
  }
}

TEST(MIPSHistoricalScheduling, RefillDuringEXLPreservesTheInterruptedEPC) {
  std::array<HistoricalTLBEntry, 48> Entries{};
  for (unsigned I = 0; I < Entries.size(); ++I)
    Entries[I].Hi = 0x10000000u + (I << 13);
  HistoricalTLB MMU{Entries, 0x55};
  for (unsigned EXL : {0u, 2u}) {
    HistoricalCP0 CPU{
        HistoricalProcessor::R4000, EXL | (1u << 22), 0, 0xdead0, {}, {}};
    CPU.Context = UINT64_C(0x1234000000800000);
    CPU.XContext = UINT64_C(0xf000000200000000);
    auto S = HistoricalSchedule::fromSnapshot(3, {}, CPU);
    ASSERT_TRUE(S);
    ASSERT_TRUE(S->beginPhysical(0x1004, 0x8c820000, false, false));
    EXPECT_FALSE(
        S->translate(MMU, 0x1004, 0x01234ab8, HistoricalMemoryAccess::Load));
    const auto &After = *S->system();
    EXPECT_EQ(After.EPC, EXL ? 0xdead0u : 0x1000u);
    EXPECT_EQ(After.Cause, EXL ? 8u : 0x80000008u);
    EXPECT_EQ(After.BadVAddr, 0x01234ab8u);
    EXPECT_EQ(After.read(10, true), 0x01234055u);
    EXPECT_EQ(After.read(4, true), UINT64_C(0x12340000008091a0));
    EXPECT_EQ(After.read(20, true), UINT64_C(0xf0000002000091a0));
    EXPECT_EQ(After.ExceptionVector, EXL ? UINT64_C(0xffffffffbfc00380)
                                         : UINT64_C(0xffffffffbfc00200));
  }
  HistoricalCP0 User{HistoricalProcessor::R4000, (1u << 25) | 16, 0, 0, {}, {}};
  EXPECT_FALSE(MMU.translate(User, 0x1200, HistoricalMemoryAccess::Load));
  ASSERT_TRUE(MMU.translate(User, 0x1200, HistoricalMemoryAccess::Fetch));
  User.Status = 1u << 21;
  EXPECT_FALSE(MMU.translate(User, 0x1200, HistoricalMemoryAccess::Fetch));
}

TEST(MIPSHistoricalScheduling, R4000RefillUsesTheInterruptedAddressMode) {
  constexpr uint32_t PC = 0x10000, Load = 0x8c820000, BEV = 1u << 22;
  constexpr uint32_t UX = 0x20, SX = 0x40, KX = 0x80;
  // The same mapped fetch and unmapped load run in each saved mode. ERL
  // makes low addresses direct, so that case faults in mapped kernel space.
  struct Case {
    uint32_t Status, Offset;
  };
  constexpr Case Modes[] = {
      {UX | SX, 0},        {KX, 0x80},          // Kernel
      {8 | UX | KX, 0},    {8 | SX, 0x80},      // Supervisor
      {16 | SX | KX, 0},   {16 | UX, 0x80},     // User
      {16 | UX | 4, 0},    {16 | KX | 4, 0x80}, // ERL selects kernel
      {16 | UX | 2, 0x180}};                    // Nested exception
  std::array<HistoricalTLBEntry, 48> Entries{};
  for (unsigned I = 0; I < Entries.size(); ++I)
    Entries[I].Hi = 0x01000000 + (I << 13);
  Entries[0] = {PC | 7, (PC >> 6) | 2, 0, 0};
  const HistoricalTLB MMU{Entries, 7};
  for (const Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Img = image(A, 3);
    Segment Code;
    Code.VA = PC;
    Code.Data = encode(A, {Load});
    Code.Size = Code.FileSz = Code.Data.size();
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Segments.push_back(std::move(Code));
    auto Ops = lift(Img, PC, {Load});
    for (const auto &[Mode, Offset] : Modes) {
      for (const auto Bootstrap : {0u, BEV}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << " Status=" << (Mode | Bootstrap));
        HistoricalCP0 CPU{
            HistoricalProcessor::R4000, Mode | Bootstrap, 0, 0x1234, {}, {}};
        MIPSSystemContext C;
        C.Historical = HistoricalSchedule::fromSnapshot(3, {}, CPU);
        ASSERT_TRUE(C.Historical);
        C.HistoricalMMU = &MMU;
        NdOpEmulator E(Img);
        E.setStrictMode(true);
        const uint64_t Address =
            (Mode & 4) ? UINT64_C(0xffffffffe0024000) : 0x24000;
        E.setRegister(mipsreg::A0, Address);
        E.setRegister(mipsreg::V0, 0x55);
        EXPECT_FALSE(execute(Ops, E, C));
        const auto &After = *C.Historical->system();
        const auto Base = Bootstrap ? UINT64_C(0xffffffffbfc00200)
                                    : UINT64_C(0xffffffff80000000);
        EXPECT_EQ(After.ExceptionVector, Base + Offset);
        EXPECT_EQ(After.EPC, Mode & 2 ? 0x1234u : PC);
        EXPECT_EQ(After.BadVAddr, Address);
        EXPECT_EQ(After.Cause, 2u << 2);
        EXPECT_EQ(After.Status, Mode | Bootstrap | 2);
        EXPECT_EQ(E.getRegister(mipsreg::V0), 0x55u);
      }
    }
  }
}

TEST(MIPSHistoricalScheduling, TranslatedLoadAndStoreReachThePhysicalFrame) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const bool Wide = isMIPS64(A);
    const uint64_t PC = Wide ? UINT64_C(0xffffffff80001000) : 0x80001000;
    auto Img = image(A, Wide ? 3 : 1);
    const uint32_t Load = 0x8c820000, Store = 0xac850004;
    Segment Code;
    Code.VA = 0x1000;
    for (uint32_t W : {Load, Store})
      for (unsigned I = 0; I < 4; ++I)
        Code.Data.push_back(W >> (8 * (archLittleEndian(A) ? I : 3 - I)));
    Code.Size = Code.FileSz = Code.Data.size();
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Segments.push_back(std::move(Code));
    Segment Frame;
    Frame.VA = 0x9000;
    Frame.Data = {0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0};
    Frame.Size = Frame.FileSz = Frame.Data.size();
    Frame.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Frame));
    std::vector<HistoricalTLBEntry> Entries(Wide ? 48 : 64);
    for (unsigned I = 0; I < Entries.size(); ++I)
      Entries[I].Hi = 0x1000000 + (I << 13);
    Entries[0] = Wide ? HistoricalTLBEntry{0x40007, 9u << 6 | 6, 0, 0}
                      : HistoricalTLBEntry{0x40000 | 7u << 6, 0x9600, 0, 0};
    const HistoricalTLB Table{Entries, 7};
    MIPSSystemContext C;
    C.HistoricalMMU = &Table;
    C.Historical = HistoricalSchedule::fromSnapshot(
        Wide ? 3 : 1, {},
        HistoricalCP0{Wide ? HistoricalProcessor::R4000
                           : HistoricalProcessor::R30xx,
                      0,
                      0,
                      0,
                      {},
                      {}});
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, 0x40000);
    E.setRegister(mipsreg::A1, 0x76543210);
    ASSERT_TRUE(execute(lift(Img, PC, {Load}), E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0),
              archLittleEndian(A) ? 0x78563412u : 0x12345678u);
    ASSERT_TRUE(execute(lift(Img, PC + 4, {Store}), E, C));
    E.setRegister(mipsreg::A0, 0x40004);
    ASSERT_TRUE(execute(lift(Img, PC, {Load}), E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x76543210u);
  }
}

TEST(MIPSHistoricalScheduling, FirstAccessUsesTheObservedReplacementFrame) {
  // These are instruction-boundary MMU snapshots. The guest ROM separately
  // executes the TLBWR. Distinct offsets and data detect stale fetches and
  // stale translations of the load independently.
  constexpr uint32_t VA = 0x01234000;
  constexpr uint32_t OldLoad = 0x8c820004, NewLoad = 0x8c820008;
  for (Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Img = image(A, 3);
    for (bool New : {false, true}) {
      Segment Page;
      Page.VA = New ? 0x4000 : 0x2000;
      Page.Data = New ? encode(A, {NewLoad, 0xdead, 0x2468})
                      : encode(A, {OldLoad, 0x1357, 0xbeef});
      Page.Size = Page.FileSz = Page.Data.size();
      Page.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Img.Segments.push_back(std::move(Page));
    }
    const auto OldOps = lift(Img, VA, {OldLoad});
    const auto NewOps = lift(Img, VA, {NewLoad});
    ASSERT_FALSE(OldOps.empty());
    ASSERT_FALSE(NewOps.empty());
    std::array<HistoricalTLBEntry, 48> Entries{};
    for (unsigned I = 0; I < Entries.size(); ++I)
      Entries[I].Hi = 0x10000000u + (I << 13);
    const HistoricalTLB MMU{Entries, 0};
    MIPSSystemContext C;
    C.HistoricalMMU = &MMU;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {}, HistoricalCP0{HistoricalProcessor::R4000, 0, 0, 0, {}, {}});
    ASSERT_TRUE(C.Historical);
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, VA);
    for (bool New : {false, true, false}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << " new=" << New);
      Entries[0] = {VA, ((New ? 0x4000u : 0x2000u) >> 6) | 0x12u, 0, 0};
      E.setRegister(mipsreg::V0, 0xa55a);
      EXPECT_FALSE(execute(New ? OldOps : NewOps, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), 0xa55au);
      ASSERT_TRUE(execute(New ? NewOps : OldOps, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), New ? 0x2468u : 0x1357u);
    }
  }
}

TEST(MIPSHistoricalScheduling, R4000ExtendedFetchUsesTheReplacementFrame) {
  constexpr uint32_t OldLoad = 0x8c820004, NewLoad = 0x8c820008;
  constexpr uint64_t Low = 0x81004000u, High = UINT64_C(0x0181004000);
  for (Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    auto Img = image(A, 3);
    addCode(Img, 0x2000, {OldLoad, 0x1357, 0xdead});
    addCode(Img, 0x4000, {NewLoad, 0xbeef, 0x2468});
    std::array<HistoricalTLBEntry, 48> Entries{};
    Entries[0] = {Low | 7, (0x2000 >> 6) | 6, 0, 0};
    Entries[1] = {High | 7, (0x4000 >> 6) | 6, 0, 0};
    const HistoricalTLB MMU{Entries, 7};
    MIPSSystemContext C;
    C.HistoricalMMU = &MMU;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {}, HistoricalCP0{HistoricalProcessor::R4000, 0x30, 0, 0, {}, {}});
    ASSERT_TRUE(C.Historical);
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    for (bool Replace : {false, true}) {
      if (Replace)
        Entries[1].Lo0 = Entries[0].Lo0;
      for (uint64_t VA : {Low, High}) {
        const bool Old = VA == Low || Replace;
        E.setRegister(mipsreg::A0, VA);
        EXPECT_FALSE(execute(lift(Img, VA, {Old ? NewLoad : OldLoad}), E, C));
        ASSERT_TRUE(execute(lift(Img, VA, {Old ? OldLoad : NewLoad}), E, C));
        EXPECT_EQ(E.getRegister(mipsreg::V0), Old ? 0x1357u : 0x2468u);
      }
    }
    // Miss only the upper tag. The saved CP0 fields must retain VPN2[39:32].
    Entries[1].Hi ^= UINT64_C(1) << 36;
    EXPECT_FALSE(execute(lift(Img, High, {OldLoad}), E, C));
    const auto &After = *C.Historical->system();
    EXPECT_EQ(After.EPC, High);
    EXPECT_EQ(After.BadVAddr, High);
    EXPECT_EQ(After.EntryHi, High | 7);
    EXPECT_EQ(After.Cause, 8u);
    EXPECT_EQ(After.ExceptionVector, UINT64_C(0xffffffff80000080));
  }
}

TEST(MIPSHistoricalScheduling, R4000ExtendedSegmentsRespectModeAndBounds) {
  using Result = HistoricalAddressResult;
  constexpr uint64_t User = UINT64_C(0x000000ffffffffff);
  constexpr uint64_t Super = UINT64_C(0x400000ffffffffff);
  constexpr uint64_t Kernel = UINT64_C(0xc00000ff7fffffff);
  constexpr uint64_t Uncached = UINT64_C(0x9000000fffffffff);
  std::array<HistoricalTLBEntry, 48> Entries{};
  const HistoricalTLB MMU{Entries, 7};
  for (const unsigned Mode : {0x80u, 0x48u, 0x30u, 0x92u, 0x94u}) {
    SCOPED_TRACE(Mode);
    HistoricalCP0 CPU{HistoricalProcessor::R4000, Mode, 0, 0, {}, {}};
    const bool IsUser = Mode == 0x30, IsSupervisor = Mode == 0x48;
    const bool IsKernel = !IsSupervisor && !IsUser;
    const auto Check = [&](uint64_t VA, Result Expected, uint64_t PA = 0) {
      for (const auto Access :
           {HistoricalMemoryAccess::Fetch, HistoricalMemoryAccess::Load,
            HistoricalMemoryAccess::Store}) {
        SCOPED_TRACE(::testing::Message() << std::hex << VA);
        const auto T = MMU.translate(CPU, VA, Access);
        ASSERT_TRUE(T);
        EXPECT_EQ(T->Result, Expected);
        EXPECT_EQ(T->Physical, PA);
      }
    };
    if (!(Mode & 4))
      Check(User, Result::Refill);
    Check(User + 1, Result::AddressError);
    Check(Super, IsUser ? Result::AddressError : Result::Refill);
    Check(Super + 1, Result::AddressError);
    Check(Kernel, IsKernel ? Result::Refill : Result::AddressError);
    Check(Kernel + 1, Result::AddressError);
    Check(Uncached, IsKernel ? Result::Mapped : Result::AddressError,
          IsKernel ? UINT64_C(0xfffffffff) : 0);
    Check(Uncached + 1, Result::AddressError);
    if (IsKernel)
      EXPECT_FALSE(MMU.translate(CPU, UINT64_C(0x8800000000000000),
                                 HistoricalMemoryAccess::Load));
    if (Mode & 4)
      Check(0x2456, Result::Mapped, 0x2456);
  }
  // KX alone cannot enable extended addressing for user or supervisor code.
  for (uint32_t Mode : {0u, 0x90u, 0x88u, 0x32u, 0x34u}) {
    HistoricalCP0 CPU{HistoricalProcessor::R4000, Mode, 0, 0, {}, {}};
    EXPECT_FALSE(MMU.translate(CPU, User, HistoricalMemoryAccess::Load));
  }
}

// The VR4300 cross-page probe maps the odd virtual page two physical frames
// after the even page. Put a different legal slot in the intervening frame.
TEST(MIPSHistoricalScheduling, SplitTLBTranslatesTheBranchSlotIndependently) {
  constexpr uint32_t VA = 0x0dea0000, PC = VA + 0xffc;
  constexpr uint32_t Branch = 0x10000001, Slot = 0x24420100,
                     Target = 0x24420200;
  constexpr uint32_t OldSlot = 0x24420010;
  for (Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(unsigned(A));
    auto Img = image(A, 3);
    addCode(Img, 0x2ffc, {Branch});
    addCode(Img, 0x3000, {OldSlot});
    addCode(Img, 0x4000, {Slot, Target});
    std::array<HistoricalTLBEntry, 48> Entries{};
    Entries[11] = {VA, (0x2000 >> 6) | 0x16, (0x3000 >> 6) | 0x16, 0};
    const HistoricalTLB MMU{Entries, 0};
    MIPSSystemContext C;
    C.HistoricalMMU = &MMU;
    C.Historical = HistoricalSchedule::fromSnapshot(
        3, {}, HistoricalCP0{HistoricalProcessor::R4000, 0, 0, 0, {}, {}});
    ASSERT_TRUE(C.Historical);
    NdOpEmulator E(Img);
    E.setStrictMode(true);
    E.setRegister(mipsreg::V0, 0);
    const auto Old = lift(Img, PC, {Branch, OldSlot});
    const auto New = lift(Img, PC, {Branch, Slot});
    ASSERT_FALSE(New.empty());
    ASSERT_EQ(New.back().Opcode, NdOp::BRANCH);
    ASSERT_TRUE(New.back().Inputs[0].isConst());
    const auto Destination = New.back().Inputs[0].Offset;
    ASSERT_EQ(Destination, PC + 8);
    ASSERT_TRUE(execute(Old, E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x10u);

    Entries[11].Lo1 = (0x4000 >> 6) | 0x16;
    E.setRegister(mipsreg::V0, 0);
    EXPECT_FALSE(execute(Old, E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0u);
    EXPECT_FALSE(C.Historical->system()->ExceptionVector);
    ASSERT_TRUE(execute(New, E, C));
    ASSERT_TRUE(execute(lift(Img, Destination, {Target}), E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x300u);

    Entries[11].Lo1 &= ~2u; // Invalidate only the slot's page.
    EXPECT_FALSE(execute(New, E, C));
    const auto &CPU = *C.Historical->system();
    EXPECT_EQ(CPU.Cause, 0x80000008u);
    EXPECT_EQ(CPU.EPC, PC);
    EXPECT_EQ(CPU.BadVAddr, PC + 4);
    EXPECT_EQ(CPU.ExceptionVector, UINT64_C(0xffffffff80000180));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x300u);
  }
}

TEST(MIPSHistoricalScheduling, BoardLineSampleStaysAtTheObservedIssuePC) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const bool Wide = isMIPS64(A);
    auto S = HistoricalSchedule::fromSnapshot(
        Wide ? 3 : 1, {},
        HistoricalCP0{Wide ? HistoricalProcessor::R4000
                           : HistoricalProcessor::R30xx,
                      0x401,
                      0,
                      0,
                      {},
                      {}});
    ASSERT_TRUE(S);
    ASSERT_TRUE(S->setHardwareInterrupts(1, 0x1204));
    EXPECT_FALSE(S->setHardwareInterrupts(0, 0x1208));
    EXPECT_FALSE(S->beginPhysical(0x1208, 0, false));
    EXPECT_FALSE(S->enterFetchAddressError(0x1205));
    EXPECT_EQ(S->system()->Cause, 0x400u);
    ASSERT_TRUE(S->beginPhysical(0x1204, 0, false, true));
    ASSERT_TRUE(S->abort());
    EXPECT_FALSE(S->beginPhysical(0x1200, 0, false));
    ASSERT_TRUE(S->beginPhysical(0x1204, 0, false, true));
    ASSERT_TRUE(S->enterException(0x1204, 0));
    EXPECT_EQ(S->system()->EPC, 0x1200u);
    EXPECT_EQ(S->system()->Cause, 0x80000400u);
    EXPECT_TRUE(S->setHardwareInterrupts(0, *S->system()->ExceptionVector));
  }
}

TEST(MIPSHistoricalScheduling, ColdResetRequiresObservedUnknownRegisters) {
  for (auto P : {HistoricalProcessor::R30xx, HistoricalProcessor::R4000}) {
    const bool R3000 = P == HistoricalProcessor::R30xx;
    constexpr uint32_t BEV = 1u << 22;
    HistoricalCP0 Sample{P, BEV | (R3000 ? 0u : 4u), 0x100, 0x88, {}, {}};
    auto S = HistoricalSchedule::fromColdReset(Sample);
    ASSERT_TRUE(S);
    EXPECT_EQ(S->system()->Cause, 0x100u);
    EXPECT_EQ(S->system()->EPC, 0x88u);
    EXPECT_EQ(S->system()->ExceptionVector,
              R3000 ? UINT64_C(0xbfc00000) : UINT64_C(0xffffffffbfc00000));
    const uint64_t PC = *S->system()->ExceptionVector;
    const HistoricalTLB UnknownTable{{}, 0};
    EXPECT_FALSE(S->beginPhysical(PC + 4, 0, false));
    EXPECT_FALSE(S->beginPhysical(PC, 0, false, false));
    ASSERT_TRUE(S->beginPhysical(PC, 0, false));
    EXPECT_EQ(S->translate(UnknownTable, PC, PC, HistoricalMemoryAccess::Fetch),
              UINT64_C(0x1fc00000));
    ASSERT_TRUE(S->abort());
    EXPECT_FALSE(S->beginPhysical(PC + 4, 0, false));
    ASSERT_TRUE(S->beginPhysical(PC, 0, false));
    ASSERT_TRUE(S->retire(PC));
    EXPECT_TRUE(S->beginPhysical(PC + 4, 0, false));
    Sample.Status ^= BEV;
    EXPECT_FALSE(HistoricalSchedule::fromColdReset(Sample));
  }
}

// IDT AN-93, figures 1 and 2, reports bus addresses and 32-bit words.
// Either byte serialization represents the listing. The CP0 values are
// fixture inputs since the logic analyzer sampled the external bus.
TEST(MIPSHistoricalScheduling, R3051ResetVectorFetchesMatchBusCapture) {
  constexpr uint32_t Jump = 0x0bf00088, Nop = 0, Lui = 0x3c020010;
  for (const Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    auto Img = image(A, 1);
    addCode(Img, 0x1fc00000, {Jump, Nop});
    addCode(Img, 0x1fc00220, {Lui});

    MIPSSystemContext C;
    const HistoricalTLB NoMappedPages{{}, 0};
    C.HistoricalMMU = &NoMappedPages;
    C.Historical = HistoricalSchedule::fromColdReset(
        {HistoricalProcessor::R30xx, 1u << 22, 0, 0, {}, {}});
    ASSERT_TRUE(C.Historical);
    ASSERT_TRUE(C.Historical->system()->ExceptionVector);
    const auto PC = *C.Historical->system()->ExceptionVector;
    const auto Ops = lift(Img, PC, {Jump, Nop});
    ASSERT_FALSE(Ops.empty());
    const auto &Branch = Ops.back();
    ASSERT_EQ(Branch.Opcode, NdOp::BRANCH);
    ASSERT_TRUE(Branch.Inputs[0].isConst());
    const auto Target = Branch.Inputs[0].Offset;
    EXPECT_EQ(Target, 0xbfc00220u);
    std::vector<uint64_t> Retired;
    for (const auto &Op : Ops)
      if (Op.Opcode == NdOp::INTRINSIC &&
          Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSHistoricalRetire))
        Retired.push_back(Op.Addr);
    EXPECT_EQ(Retired, (std::vector<uint64_t>{0xbfc00000, 0xbfc00004}));

    NdOpEmulator E(Img);
    E.setStrictMode(true);
    ASSERT_TRUE(execute(Ops, E, C));
    ASSERT_TRUE(execute(lift(Img, Target, {Lui}), E, C));
    EXPECT_EQ(E.getRegister(mipsreg::V0), 0x00100000u);
  }
}

// Crescent Heart R4300 and R4XXX manuals, figure 4.3 in each. The timing
// acquisitions identify the first vector read; these listings supply the
// words fetched from the vector, its delay slot, and the branch target.
TEST(MIPSHistoricalScheduling, R4300AndR4600BootFetchesMatchBusCaptures) {
  constexpr uint64_t ResetPC = UINT64_C(0xffffffffbfc00000);
  constexpr uint32_t Initial = 0x12345678;
  struct BootFetch {
    uint32_t Jump, Slot;
    uint64_t Target;
    uint32_t TargetWord, ExpectedK0, ExpectedV0;
  };
  constexpr BootFetch Captures[] = {
      {0x0bf00158, 0x0000d021, UINT64_C(0xffffffffbfc00560), 0x00000021, 0,
       Initial},
      {0x0bf000e2, 0x00000000, UINT64_C(0xffffffffbfc00388), 0x3c022001,
       Initial, 0x20010000}};
  for (const auto &Capture : Captures) {
    SCOPED_TRACE(Capture.Target);
    for (const auto A : {Arch::MIPS64, Arch::MIPS64EL}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      auto Img = image(A, 3);
      addCode(Img, 0x1fc00000, {Capture.Jump, Capture.Slot});
      addCode(Img, Capture.Target & 0x1fffffff, {Capture.TargetWord});
      const auto Ops = lift(Img, ResetPC, {Capture.Jump, Capture.Slot});
      ASSERT_FALSE(Ops.empty());
      ASSERT_EQ(Ops.back().Opcode, NdOp::BRANCH);
      ASSERT_TRUE(Ops.back().Inputs[0].isConst());
      const auto Destination = Ops.back().Inputs[0].Offset;
      ASSERT_EQ(Destination, Capture.Target);

      NdOpEmulator E(Img);
      E.setStrictMode(true);
      E.setRegister(mipsreg::K0, Initial);
      E.setRegister(mipsreg::V0, Initial);
      MIPSSystemContext C;
      // The probe supplied ROM words. The R4000 profile supplies the common
      // MIPS III reset vector and unmapped kseg1 translation for this replay.
      const HistoricalTLB NoMappedPages{{}, 0};
      C.HistoricalMMU = &NoMappedPages;
      C.Historical = HistoricalSchedule::fromColdReset(
          {HistoricalProcessor::R4000, (1u << 22) | 4u, 0, 0, {}, {}});
      ASSERT_TRUE(C.Historical);
      ASSERT_TRUE(execute(Ops, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::K0), Capture.ExpectedK0);
      const auto TargetOps = lift(Img, Destination, {Capture.TargetWord});
      ASSERT_FALSE(TargetOps.empty());
      ASSERT_TRUE(execute(TargetOps, E, C));
      EXPECT_EQ(E.getRegister(mipsreg::V0), Capture.ExpectedV0);
    }
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

//===- SymX87Tests.cpp - Concrete x87 decisions in symbolic paths --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X86/X87GuardedValue.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/APInt.h"

#include <array>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt x87Bits(uint64_t Significand, uint16_t SignExponent) {
  return llvm::APInt(80, Significand) | (llvm::APInt(80, SignExponent) << 64);
}

LowOp intrinsic(Intrinsic Id, NdVar Output = {}) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = Output;
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  return Op;
}

LowOp fprem() {
  LowOp Op = intrinsic(Intrinsic::X87Fprem,
                       NdVar::reg(x86reg::ST0, x86reg::FPURegSize));
  Op.addInput(NdVar::reg(x86reg::ST0, x86reg::FPURegSize));
  Op.addInput(NdVar::reg(x86reg::ST1, x86reg::FPURegSize));
  return Op;
}

LowOp ffree() {
  LowOp Op = intrinsic(Intrinsic::X87Ffree);
  Op.addInput(NdVar::reg(x86reg::ST1, x86reg::FPURegSize));
  Op.addInput(NdVar::cst(1, 1));
  return Op;
}

TEST(SymX87, ConcretePartialRemainderDeterminesC2) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  ASSERT_EQ(Exec.step(intrinsic(Intrinsic::X87Fninit)), StepResult::Unmodelled);

  // The anti-emulation PoC writes these exact 80-bit encodings to memory,
  // loads them into ST(0)/ST(1), and checks C2 after one FPREM step.
  State.write(SymSpace::Register, x86reg::ST0,
              Ctx.mkConst(x87Bits(UINT64_C(0x8000000000000001), 0x7ffe)));
  State.write(SymSpace::Register, x86reg::ST1,
              Ctx.mkConst(x87Bits(UINT64_C(0x8000000000000003), 0xffbe)));
  ASSERT_EQ(Exec.step(fprem()), StepResult::Unmodelled);

  LowOp ReadStatus =
      intrinsic(Intrinsic::X87ReadStatus, NdVar::reg(x86reg::FPU_SW, 2));
  ASSERT_EQ(Exec.step(ReadStatus), StepResult::Continue);
  const SymRef Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
  EXPECT_FALSE(Ctx.isConst(Status));
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::ST0, 10)));
  EXPECT_EQ(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x0400)),
            Ctx.mkConst(16, 0x0400));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x4300))));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x3800))));

  // The PoC saves SW in AX before FFREE/FINCSTP. Those later x87 operations
  // cannot erase the already captured C2 decision in a general register.
  LowOp SaveStatus;
  SaveStatus.Opcode = NdOp::COPY;
  SaveStatus.Output = NdVar::reg(x86reg::RAX, 2);
  SaveStatus.addInput(NdVar::reg(x86reg::FPU_SW, 2));
  ASSERT_EQ(Exec.step(SaveStatus), StepResult::Continue);
  EXPECT_EQ(Exec.step(ffree()), StepResult::Unmodelled);
  EXPECT_FALSE(
      Ctx.isConst(Ctx.mkAnd(State.read(SymSpace::Register, x86reg::FPU_SW, 2),
                            Ctx.mkConst(16, 0x0400))));
  EXPECT_EQ(Exec.step(intrinsic(Intrinsic::X87Fincstp)),
            StepResult::Unmodelled);
  SymRef AX = State.read(SymSpace::Register, x86reg::RAX, 2);
  EXPECT_FALSE(Ctx.isConst(AX));
  EXPECT_EQ(Ctx.mkAnd(AX, Ctx.mkConst(16, 0x0400)), Ctx.mkConst(16, 0x0400));
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2)));
}

TEST(SymX87, UnknownOperandMakesRemainderAndStatusUnknown) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  ASSERT_EQ(Exec.step(intrinsic(Intrinsic::X87Fninit)), StepResult::Unmodelled);
  State.write(SymSpace::Register, x86reg::ST0, Ctx.mkVar("dividend", 80));
  State.write(SymSpace::Register, x86reg::ST1,
              Ctx.mkConst(x87Bits(UINT64_C(0x8000000000000003), 0xffbe)));

  EXPECT_EQ(Exec.step(fprem()), StepResult::Unmodelled);
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::ST0, 10)));
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2)));
  EXPECT_EQ(Exec.step(intrinsic(Intrinsic::X87ReadStatus,
                                NdVar::reg(x86reg::FPU_SW, 2))),
            StepResult::Continue);
  EXPECT_EQ(Exec.unmodelledCount(), 2u);
}

TEST(SymX87, FreePopInvalidatesBothPhysicalSlotsAndCannotInventKnownTop) {
  for (unsigned Top = 0; Top != 8; ++Top)
    for (unsigned Index = 0; Index != 8; ++Index) {
      SymContext Ctx;
      SymState State(Ctx);
      SymExec Exec(Ctx, State);
      State.write(SymSpace::Register, x86reg::FPU_SW, Ctx.mkConst(16, 0x4700));
      for (unsigned Slot = 0; Slot != 8; ++Slot)
        State.write(SymSpace::Register, x86reg::stReg(Slot),
                    Ctx.mkConst(80, Slot));
      LowOp Op = intrinsic(Intrinsic::X87Ffreep);
      Op.addInput(NdVar::reg(x86reg::stReg((Top + Index) & 7), 10));
      Op.addInput(NdVar::cst(Index, 1));
      EXPECT_EQ(Exec.step(Op), StepResult::Unmodelled);
      for (unsigned Slot = 0; Slot != 8; ++Slot)
        EXPECT_EQ(Ctx.isConst(
                      State.read(SymSpace::Register, x86reg::stReg(Slot), 10)),
                  Slot != Top && Slot != ((Top + Index) & 7));
      const auto Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
      EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x3800))));
      EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x4700))));
    }
}

TEST(SymX87, DecrementDoesNotInitializeARegisterOrPublishGuessedStatus) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, x86reg::FPU_SW, Ctx.mkZero(16));
  State.write(SymSpace::Register, x86reg::ST0, Ctx.mkConst(80, 42));
  const auto Unknown = State.read(SymSpace::Register, x86reg::ST7, 10);
  EXPECT_EQ(Exec.step(intrinsic(Intrinsic::X87Fdecstp)),
            StepResult::Unmodelled);
  EXPECT_EQ(State.read(SymSpace::Register, x86reg::ST0, 10),
            Ctx.mkConst(80, 42));
  EXPECT_EQ(State.read(SymSpace::Register, x86reg::ST7, 10), Unknown);
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2)));
}

TEST(SymX87, GuardedNativeResultsAndStatusRemainExplicitlyUnknown) {
  for (auto Id : {Intrinsic::X87LoadPackedBCD, Intrinsic::X87StorePackedBCD,
                  Intrinsic::X87FptanPair, Intrinsic::X87FsincosPair,
                  Intrinsic::X87FxtractPair}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, x86reg::FPU_SW, Ctx.mkZero(16));
    State.write(SymSpace::Register, x86reg::ST0, Ctx.mkConst(80, 1));
    const unsigned Bytes = x87GuardedValueBytes(Id);
    LowOp Op = intrinsic(Id, NdVar::tmp(90000, Bytes));
    Op.addInput(NdVar::reg(x86reg::ST0, 10));
    Op.addInput(NdVar::cst(0x37f, 2));
    EXPECT_EQ(Exec.step(Op), StepResult::Unmodelled);
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Temporary, 90000, Bytes)));
    EXPECT_FALSE(
        Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2)));
    EXPECT_EQ(State.read(SymSpace::Register, x86reg::ST0, 10),
              Ctx.mkConst(80, 1));
    EXPECT_EQ(Exec.unmodelledCount(), 1u);
  }
}

TEST(SymX87, CompletedRemainderHasExactValueAndQuotientBits) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  ASSERT_EQ(Exec.step(intrinsic(Intrinsic::X87Fninit)), StepResult::Unmodelled);
  State.write(SymSpace::Register, x86reg::ST0,
              Ctx.mkConst(x87Bits(UINT64_C(0xe000000000000000), 0x4001)));
  State.write(SymSpace::Register, x86reg::ST1,
              Ctx.mkConst(x87Bits(UINT64_C(0xc000000000000000), 0x4000)));

  ASSERT_EQ(Exec.step(fprem()), StepResult::Continue);
  const SymRef Remainder = State.read(SymSpace::Register, x86reg::ST0, 10);
  const SymRef Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
  ASSERT_TRUE(Ctx.isConst(Remainder));
  EXPECT_FALSE(Ctx.isConst(Status));
  EXPECT_EQ(Ctx.constValue(Remainder),
            x87Bits(UINT64_C(0x8000000000000000), 0x3fff));
  EXPECT_EQ(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x4700)),
            Ctx.mkConst(16, 0x4000));
  EXPECT_FALSE(Ctx.isConst(Ctx.mkAnd(Status, Ctx.mkConst(16, 0x3800))));
}

TEST(SymX87, StateRestoreInvalidatesPriorKnownC2) {
  constexpr std::array Restores{
      Intrinsic::Fxrstor, Intrinsic::Fxrstor64Mem, Intrinsic::Xrstor,
      Intrinsic::Xrstors, Intrinsic::Xrstor64,     Intrinsic::Xrstors64,
  };
  for (Intrinsic Id : Restores) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    ASSERT_EQ(Exec.step(intrinsic(Intrinsic::X87Fninit)),
              StepResult::Unmodelled);
    State.write(SymSpace::Register, x86reg::ST0,
                Ctx.mkConst(x87Bits(UINT64_C(0x8000000000000001), 0x7ffe)));
    State.write(SymSpace::Register, x86reg::ST1,
                Ctx.mkConst(x87Bits(UINT64_C(0x8000000000000003), 0xffbe)));
    ASSERT_EQ(Exec.step(fprem()), StepResult::Unmodelled);
    EXPECT_EQ(Ctx.mkAnd(State.read(SymSpace::Register, x86reg::FPU_SW, 2),
                        Ctx.mkConst(16, 0x0400)),
              Ctx.mkConst(16, 0x0400));

    LowOp Restore = intrinsic(Id);
    Restore.addInput(NdVar::cst(0x402000, 8));
    if (Id == Intrinsic::Xrstor || Id == Intrinsic::Xrstors ||
        Id == Intrinsic::Xrstor64 || Id == Intrinsic::Xrstors64) {
      Restore.addInput(NdVar::reg(x86reg::RAX, 4));
      Restore.addInput(NdVar::reg(x86reg::RDX, 4));
    }
    EXPECT_EQ(Exec.step(Restore), StepResult::Unmodelled);
    EXPECT_FALSE(
        Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2)));
    EXPECT_FALSE(
        Ctx.isConst(Ctx.mkAnd(State.read(SymSpace::Register, x86reg::FPU_SW, 2),
                              Ctx.mkConst(16, 0x0400))));
    EXPECT_FALSE(
        Ctx.isConst(State.read(SymSpace::Register, x86reg::FPU_CW, 2)));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, x86reg::ST0, 10)));
  }
}

} // namespace

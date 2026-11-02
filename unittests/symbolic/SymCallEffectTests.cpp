//===- SymCallEffectTests.cpp - Explicit call state results ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check complete call effects with scalar outputs, memory postconditions,
/// partial register writes, pre-call inputs and invalid contracts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"

#include <limits>
#include <stdexcept>

using namespace neverd;
using namespace neverd::symbolic;

namespace {
LowOp call(NdOp Opcode = NdOp::CALL, NdVar Output = {}) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.addInput(NdVar::reg(0, 8));
  return Op;
}
} // namespace

TEST(SymCallEffect,
     SuppliesMultipleOutputsWithoutPreservingOtherVolatileBytes) {
  for (auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, 0, Ctx.mkConst(64, 0x2000));
    State.write(SymSpace::Register, 8, Ctx.mkConst(64, 0xabcdef));
    State.write(SymSpace::Register, 16, Ctx.mkConst(64, 0x1234));
    State.write(SymSpace::Register, 24, Ctx.mkConst(64, 0x5678));
    Exec.setCallPreservedRegisters({{24, 8}});
    const auto Address = Ctx.mkConst(64, 0x3000);
    State.store(Address, Ctx.mkConst(64, 0xfedcba));

    const auto Status = Ctx.mkFreshVar(64, "status");
    const auto Flags = Ctx.mkOr(Ctx.mkAnd(Status, Ctx.mkConst(64, 0xff)),
                                Ctx.mkConst(64, 0x100));
    SymCallEffect Effect;
    Effect.Memory = SymCallMemoryEffect::Preserve;
    Effect.Registers = {{8, Ctx.mkConst(64, 19)}, {32, Flags}};
    EXPECT_EQ(Exec.step(call(Opcode), &Effect), StepResult::Continue);
    EXPECT_EQ(Exec.branchTarget(), Ctx.mkConst(64, 0x2000));
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, 19));
    EXPECT_EQ(State.read(SymSpace::Register, 32, 8), Flags);
    EXPECT_EQ(State.read(SymSpace::Register, 24, 8), Ctx.mkConst(64, 0x5678));
    EXPECT_EQ(State.load(Address, 8), Ctx.mkConst(64, 0xfedcba));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 0, 8)));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 16, 8)));
    EXPECT_EQ(Exec.callHavocCount(), 0u);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
    for (unsigned Value = 0; Value != 256; ++Value)
      EXPECT_EQ(Ctx.eval(State.read(SymSpace::Register, 32, 8),
                         {llvm::APInt(64, Value)})
                    .getZExtValue(),
                Value | 0x100u);
  }
}

TEST(SymCallEffect, RegisterAndMemoryResultsUseCapturedPreCallInputs) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Pointer = Ctx.mkConst(64, 0x4000);
  const auto Input = Ctx.mkVar("input", 64);
  State.write(SymSpace::Register, 0, Pointer);
  State.write(SymSpace::Register, 8, Input);
  SymCallEffect Effect;
  Effect.Writes = {{State.read(SymSpace::Register, 0, 8), Input}};
  Effect.Registers = {{0, State.read(SymSpace::Register, 8, 8)},
                      {8, State.read(SymSpace::Register, 0, 8)}};
  Effect.ReturnValue = Ctx.mkConst(16, 0xbeef);
  EXPECT_EQ(Exec.step(call(NdOp::CALL, NdVar::reg(0, 2)), &Effect),
            StepResult::Continue);
  EXPECT_EQ(Exec.branchTarget(), Pointer);
  EXPECT_EQ(State.load(Pointer, 8), Input);
  EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Pointer);
  const auto Result = State.read(SymSpace::Register, 0, 8);
  for (uint64_t Value : {uint64_t(0), uint64_t(0x123456789abcdef0),
                         std::numeric_limits<uint64_t>::max()})
    EXPECT_EQ(Ctx.eval(Result, {llvm::APInt(64, Value)}).getZExtValue(),
              (Value & ~uint64_t(0xffff)) | 0xbeef);
}

TEST(SymCallEffect, OrderedPartialWritesRespectBothByteOrders) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, 8, Ctx.mkConst(64, 0x0123456789abcdef));
    Exec.setCallPreservedRegisters({{8, 8}});
    SymCallEffect Effect;
    Effect.Registers = {{10, Ctx.mkConst(16, 0xa1b2)},
                        {11, Ctx.mkConst(8, 0xc3)}};
    EXPECT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
    const uint64_t Expected = Order == llvm::endianness::little
                                  ? 0x01234567c3b2cdef
                                  : 0x0123a1c389abcdef;
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, Expected));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 16, 8)));
  }
}

TEST(SymCallEffect, RegisterResultsDoNotPreserveUnknownMemory) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Address = Ctx.mkConst(64, 0x5000);
  State.store(Address, Ctx.mkConst(32, 0x11223344));
  SymCallEffect Effect;
  Effect.Registers = {{8, Ctx.mkConst(64, 7)}};
  EXPECT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
  EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, 7));
  EXPECT_FALSE(Ctx.isConst(State.load(Address, 4)));
  EXPECT_TRUE(State.memoryIsUnknown());
}

TEST(SymCallEffect, InvalidRegisterRangesRejectBeforeAnyStateMutation) {
  SymContext Ctx;
  const SymCallRegisterWrite Invalid[] = {
      {8, {}},
      {8, Ctx.mkConst(7, 0)},
      {std::numeric_limits<uint64_t>::max(), Ctx.mkConst(16, 0)},
      {8, Ctx.mkZero(65536 * 8)}};
  for (auto Bad : Invalid) {
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    const auto Value = Ctx.mkConst(64, 0x12345678);
    const auto Address = Ctx.mkConst(64, 0x6000);
    State.write(SymSpace::Register, 8, Value);
    State.store(Address, Value);
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Value));
    const auto Live = State.numLiveBytes();
    SymCallEffect Effect;
    Effect.Registers = {{8, Ctx.mkZero(64)}, Bad};
    // Capture after constructing the proposed contract, before admission.
    const auto BeforeNodes = Ctx.numNodes();
    EXPECT_THROW(Exec.step(call(NdOp::INDIR_CALL), &Effect),
                 std::invalid_argument);
    EXPECT_EQ(State.numLiveBytes(), Live);
    EXPECT_EQ(Ctx.numNodes(), BeforeNodes);
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Value);
    EXPECT_EQ(State.load(Address, 8), Value);
    EXPECT_EQ(State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
              Value);
    EXPECT_FALSE(Exec.branchTarget().isValid());
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
  }
}

TEST(SymCallEffect, NonCallOperationsIgnoreTheCallContract) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  SymCallEffect Effect;
  Effect.Registers = {{0, {}}};
  Effect.MemoryAfter.emplace();
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::reg(0, 8);
  Op.addInput(NdVar::cst(9, 8));
  EXPECT_EQ(Exec.step(Op, &Effect), StepResult::Continue);
  EXPECT_EQ(State.read(SymSpace::Register, 0, 8), Ctx.mkConst(64, 9));
}

TEST(SymCallEffect, MemoryPostconditionKeepsCallerFactsAndForgetsScratch) {
  for (auto Policy :
       {SymCallMemoryEffect::Havoc, SymCallMemoryEffect::Preserve}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    const auto Frame = Ctx.mkVar("frame", 64);
    const auto Object = Ctx.mkVar("object", 64);
    const auto ReturnAddress = Ctx.mkConst(64, 0x4100);
    const auto Scratch = State.load(Frame, 8);
    const auto CallerInput =
        State.load(Ctx.mkAdd(Frame, Ctx.mkConst(64, 8)), 8);
    const auto ObjectInput = State.load(Object, 8);
    State.write(SymSpace::Register, 24, ReturnAddress);
    Exec.setCallPreservedRegisters({{24, 8}});
    SymCallEffect Effect;
    Effect.Memory = Policy;
    Effect.MemoryAfter = SymState::MemoryKnowledge{&Ctx,
                                                   State.byteOrder(),
                                                   {{Frame, 8, CallerInput},
                                                    {Frame, 16, ReturnAddress},
                                                    {Object, 0, ObjectInput}}};
    ASSERT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
    EXPECT_EQ(State.load(Ctx.mkAdd(Frame, Ctx.mkConst(64, 8)), 8), CallerInput);
    EXPECT_EQ(State.load(Ctx.mkAdd(Frame, Ctx.mkConst(64, 16)), 8),
              ReturnAddress);
    EXPECT_EQ(State.load(Object, 8), ObjectInput);
    EXPECT_EQ(State.read(SymSpace::Register, 24, 8), ReturnAddress);
    const auto NewScratch = State.load(Frame, 8);
    EXPECT_NE(NewScratch, Scratch);
    EXPECT_FALSE(Ctx.isConst(NewScratch));
    EXPECT_TRUE(State.memoryInputOrigins(NewScratch).empty());
    const auto Unread = State.load(Ctx.mkAdd(Frame, Ctx.mkConst(64, 32)), 8);
    EXPECT_FALSE(Ctx.isConst(Unread));
    EXPECT_TRUE(State.memoryInputOrigins(Unread).empty());
    EXPECT_TRUE(State.memoryIsUnknown());
    EXPECT_EQ(Exec.callHavocCount(), 0u);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
  }
}

TEST(SymCallEffect, ConditionalMemoryResultsKeepPreCallValuesAndByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    SymExec Exec(Ctx, State);
    const auto Success = Ctx.mkVar("success", 1);
    const auto Prior = Ctx.mkConst(64, 0x0123456789abcdef);
    const auto Result =
        Ctx.mkIte(Success, Ctx.mkConst(64, 0xfedcba9876543210), Prior);
    const auto Slot = Ctx.mkConst(64, 0x5000);
    ASSERT_TRUE(State.store(Slot, Prior));
    SymCallEffect Effect;
    Effect.MemoryAfter =
        SymState::MemoryKnowledge{&Ctx, Order, {{{}, 0x5000, Result}}};
    Effect.Writes = {{Ctx.mkConst(64, 0x5002), Ctx.mkConst(16, 0xbeef)}};
    ASSERT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
    for (unsigned Value = 0; Value != 2; ++Value) {
      uint64_t Expected = Value ? 0xfedcba9876543210 : 0x0123456789abcdef;
      const unsigned Shift = Order == llvm::endianness::little ? 16 : 32;
      Expected = (Expected & ~(uint64_t(0xffff) << Shift)) |
                 (uint64_t(0xbeef) << Shift);
      EXPECT_EQ(
          Ctx.eval(State.load(Slot, 8), {llvm::APInt(1, Value)}).getZExtValue(),
          Expected);
    }
  }
}

TEST(SymCallEffect, CapturesMemoryCallTargetBeforeReplacingMemory) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Slot = Ctx.mkConst(64, 0x7000);
  const auto Destination = Ctx.mkConst(64, 0x4100);
  ASSERT_TRUE(State.store(Slot, Destination));
  SymCallEffect Effect;
  Effect.MemoryAfter = SymState::MemoryKnowledge{&Ctx, State.byteOrder(), {}};
  LowOp Op;
  Op.Opcode = NdOp::INDIR_CALL;
  Op.addInput(NdVar::ram(0x7000, 8));
  ASSERT_EQ(Exec.step(Op, &Effect), StepResult::Continue);
  EXPECT_EQ(Exec.branchTarget(), Destination);
  EXPECT_FALSE(Ctx.isConst(State.load(Slot, 8)));
}

TEST(SymCallEffect, MemoryPostconditionDoesNotGrantAliasSeparation) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto P = Ctx.mkVar("p", 64);
  const auto Q = Ctx.mkVar("q", 64);
  const auto Value = Ctx.mkConst(64, 19);
  SymCallEffect Effect;
  Effect.MemoryAfter =
      SymState::MemoryKnowledge{&Ctx, State.byteOrder(), {{P, 0, Value}}};
  Effect.Writes = {{Q, Ctx.mkConst(64, 21)}};
  ASSERT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
  EXPECT_EQ(State.load(Q, 8), Ctx.mkConst(64, 21));
  EXPECT_FALSE(Ctx.isConst(State.load(P, 8)));
}

TEST(SymCallEffect, RepeatedMemoryContractsCreateDistinctUnknownEpochs) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Address = Ctx.mkConst(64, 0x8000);
  SymCallEffect Effect;
  Effect.MemoryAfter = SymState::MemoryKnowledge{&Ctx, State.byteOrder(), {}};
  ASSERT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
  const auto First = State.load(Address, 8);
  EXPECT_EQ(State.load(Address, 8), First);
  ASSERT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
  const auto Second = State.load(Address, 8);
  EXPECT_NE(Second, First);
  EXPECT_EQ(State.load(Address, 8), Second);
}

TEST(SymCallEffect, RejectsMalformedMemoryBeforeChangingAnyState) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SymContext Ctx, Other;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    const auto Value = Ctx.mkConst(64, 0xabcdef);
    const auto Address = Ctx.mkConst(64, 0x6000);
    State.write(SymSpace::Register, 8, Value);
    ASSERT_TRUE(State.store(Address, Value));
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Value));
    SymCallEffect Effect;
    Effect.Registers = {{8, Ctx.mkZero(64)}};
    Effect.MemoryAfter = SymState::MemoryKnowledge{
        &Ctx, State.byteOrder(), {{{}, 0x6000, Value}}};
    auto &Knowledge = *Effect.MemoryAfter;
    switch (Case) {
    case 0:
      Knowledge.Owner = &Other;
      break;
    case 1:
      Knowledge.Order = llvm::endianness::big;
      break;
    case 2:
      Knowledge.Facts.push_back({{}, 0x6000, Ctx.mkZero(8)});
      break;
    case 3:
      Knowledge.Facts.front().Value = {};
      break;
    case 4:
      Knowledge.Facts.front().Value = Ctx.mkTrue();
      break;
    case 5:
      Knowledge.Facts.front().Offset = std::numeric_limits<uint64_t>::max();
      break;
    }
    const auto Live = State.numLiveBytes();
    EXPECT_THROW(Exec.step(call(NdOp::INDIR_CALL), &Effect),
                 std::invalid_argument);
    EXPECT_EQ(State.numLiveBytes(), Live);
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Value);
    EXPECT_EQ(State.load(Address, 8), Value);
    EXPECT_EQ(State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
              Value);
    EXPECT_FALSE(Exec.branchTarget().isValid());
    EXPECT_FALSE(State.memoryIsUnknown());
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
  }
}

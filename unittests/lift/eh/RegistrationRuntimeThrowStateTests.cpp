//===- RegistrationRuntimeThrowStateTests.cpp - Direct object proof ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Require actual table arguments and initialized private scalar object bytes.
//===----------------------------------------------------------------------===//
#include "RegistrationRealignedTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {
void checkDirectThrowObject(bool Callback) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = Callback ? makeRealignedRegistrationFrame()
                             : makeCxxCatchContinuation();
    auto &EH = *Function.ExceptionMetadata;
    if (Callback) {
      EH.CodeRange.End = 0x1200;
      EH.Cxx->MaxState = 2;
      EH.Cxx->UnwindMap.resize(2, {-1, 0, CxxUnwindAction::ActionKind::None});
      CxxTryBlock Try;
      Try.TryLow = Try.TryHigh = 0;
      Try.CatchHigh = 1;
      CxxCatchHandler Catch;
      Catch.HandlerVA = 0x1100;
      Try.Handlers.push_back(Catch);
      EH.Cxx->TryBlocks.push_back(Try);
      Function.Blocks.resize(5);
      Function.Blocks.back().Id = 4;
      Function.Blocks.back().StartAddr = 0x1100;
    } else {
      auto &Stores = EH.Registration->TryLevelStores;
      Stores.erase(Stores.begin() + 1);
    }
    auto &Block = Function.Blocks[Callback ? 4 : 2];
    const auto Begin = Block.StartAddr;
    Block.Ops.clear();
    Block.Succs.clear();
    Block.InstructionBoundaries = {{Begin, 3}, {Begin + 3, 3}, {Begin + 6, 5}};
    Block.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Block.EndAddr = Begin + 11;
    const auto SP = NdVar::reg(x86reg::RSP, 4);
    const auto Object = NdVar::reg(x86reg::RCX, 4);
    if (Callback)
      emitOp(Block, Begin, NdOp::INT_SUB, SP, {SP, NdVar::cst(16, 4)});
    else
      emitOp(Block, Begin, NdOp::INT_ADD, SP,
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-40), 4)});
    emitOp(Block, Begin, NdOp::INT_ADD, Object,
           {SP, NdVar::cst(Mutation == 7 ? (Callback ? 16 : 28)
                                         : (Callback ? 8 : 16),
                           4)});
    if (Mutation != 1)
      emitOp(Block, Begin, NdOp::STORE, {},
             {Object, Mutation == 3 ? NdVar::reg(x86reg::RBP, 4)
                                    : NdVar::cst(7, Mutation == 2 ? 1 : 4)});
    emitOp(Block, Begin + 3, NdOp::STORE, {},
           {SP, Mutation == 6 ? NdVar::cst(0, 4) : Object});
    emitOp(Block, Begin + 3, NdOp::INT_ADD, NdVar::tmp(901, 4),
           {SP, NdVar::cst(4, 4)});
    emitOp(Block, Begin + 3, NdOp::STORE, {},
           {NdVar::tmp(901, 4), NdVar::cst(Mutation == 4 ? 0x3004 : 0x3000,
                                           Mutation == 5 ? 1 : 4)});
    emitOp(Block, Begin + 6, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    RegistrationCalleeFrameContract Contract;
    Contract.CalleeKind = RegistrationCalleeFrameContract::Kind::RuntimeThrow;
    Contract.Target = 0x2100;
    Contract.DoesNotReturn = true;
    Contract.RuntimeThrowInfos.push_back(
        {0x3000, 0x5000, Mutation == 8 ? 8u : 4u});
    if (Mutation == 9)
      Contract.RuntimeThrowInfos.push_back(Contract.RuntimeThrowInfos.front());
    const std::vector Contracts{Contract};
    const auto State = analyzeRegistrationStates(Function, 0, 0, &Contracts);
    EXPECT_EQ(State.CallFrameEffectsComplete, Mutation == 0);
    if (Mutation)
      continue;
    ASSERT_EQ(State.CallFrameEffects.size(), 1u);
    ASSERT_TRUE(State.CallFrameEffects.front().RuntimeThrow);
    const auto &Throw = *State.CallFrameEffects.front().RuntimeThrow;
    EXPECT_FALSE(Throw.isRethrow());
    EXPECT_EQ(Throw.ThrowInfoVA, 0x3000u);
    EXPECT_EQ(Throw.ObjectSize, 4u);
    EXPECT_EQ(Throw.ObjectOffset, Callback ? -8 : -24);
    EXPECT_EQ(Throw.CallbackVA, Callback ? 0x1100u : 0u);
  }
}
TEST(RegistrationState, DirectThrowRequiresInitializedParentScalarStorage) {
  checkDirectThrowObject(false);
}
TEST(RegistrationState, DirectThrowRequiresCurrentCallbackScalarStorage) {
  checkDirectThrowObject(true);
}
} // namespace

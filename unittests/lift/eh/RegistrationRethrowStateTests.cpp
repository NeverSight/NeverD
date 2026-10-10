//===- RegistrationRethrowStateTests.cpp - Active catch proof ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject rethrows without a live catch and exact initialized arguments.
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {
TEST(RegistrationState, RethrowRequiresALiveCatchAndHasNoNewObject) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxCatchContinuation();
    RegistrationCalleeFrameContract Contract;
    Contract.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateRethrow;
    Contract.Target = 0x2100;
    Contract.DoesNotReturn = true;
    auto &Block = F.Blocks[Mutation == 1 ? 2 : 5];
    Block.Ops.clear();
    Block.InstructionBoundaries = {{Block.StartAddr, 3},
                                   {Block.StartAddr + 3, 5}};
    Block.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Block.EndAddr = Block.StartAddr + 8;
    emitOp(Block, Block.StartAddr, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Block, Block.StartAddr + 3, NdOp::CALL, {},
           {NdVar::cst(Contract.Target, 4)});
    if (Mutation == 1) {
      auto &Stores = F.ExceptionMetadata->Registration->TryLevelStores;
      Stores.erase(Stores.begin() + 1);
    }
    if (Mutation == 2)
      Contract.ThrownTypeVA = 0x3000;
    if (Mutation == 3)
      Contract.ThrownObjectSize = 4;
    const std::vector Contracts{Contract};
    const auto Result = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(Result.CallFrameEffectsComplete, Mutation == 0);
    if (Mutation == 0) {
      ASSERT_EQ(Result.CallFrameEffects.size(), 1u);
      EXPECT_TRUE(Result.CallFrameEffects.front().DoesNotReturn);
      EXPECT_TRUE(Result.CxxCatchObjects.empty());
      EXPECT_TRUE(Result.CxxContinuations.empty());
    }
  }
}
TEST(RegistrationState, DirectRethrowRequiresBothInitializedNullWords) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxCatchContinuation();
    RegistrationCalleeFrameContract Contract;
    Contract.CalleeKind = RegistrationCalleeFrameContract::Kind::RuntimeRethrow;
    Contract.Target = 0x2100;
    Contract.DoesNotReturn = true;
    auto &Block = F.Blocks[Mutation == 6 ? 2 : 5];
    Block.Ops.clear();
    Block.InstructionBoundaries = {{Block.StartAddr, 3},
                                   {Block.StartAddr + 3, 3},
                                   {Block.StartAddr + 6, 3},
                                   {Block.StartAddr + 9, 5}};
    Block.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Block.EndAddr = Block.StartAddr + 14;
    emitOp(Block, Block.StartAddr, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    if (Mutation != 3)
      emitOp(Block, Block.StartAddr + 3, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), Mutation == 4
                                              ? NdVar::reg(x86reg::RBP, 4)
                                              : NdVar::cst(Mutation == 1, 4)});
    emitOp(Block, Block.StartAddr + 6, NdOp::INT_ADD, NdVar::tmp(900, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(
        Block, Block.StartAddr + 6, NdOp::STORE, {},
        {NdVar::tmp(900, 4), NdVar::cst(Mutation == 2, Mutation == 5 ? 1 : 4)});
    emitOp(Block, Block.StartAddr + 9, NdOp::CALL, {},
           {NdVar::cst(Contract.Target, 4)});
    if (Mutation == 6) {
      auto &Stores = F.ExceptionMetadata->Registration->TryLevelStores;
      Stores.erase(Stores.begin() + 1);
    }
    const std::vector Contracts{Contract};
    const auto Result = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(Result.CallFrameEffectsComplete, Mutation == 0);
    EXPECT_EQ(Result.CallFrameEffects.size(), unsigned(Mutation == 0));
  }
}
TEST(RegistrationState, DirectRethrowUsesTheActiveCallbackArgumentStack) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeRealignedRegistrationFrame();
    auto &EH = *F.ExceptionMetadata;
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
    F.Blocks.resize(5);
    auto &Block = F.Blocks.back();
    Block.Id = 4;
    Block.StartAddr = 0x1100;
    Block.EndAddr = 0x110e;
    Block.InstructionBoundaries = {
        {0x1100, 3}, {0x1103, 3}, {0x1106, 3}, {0x1109, 5}};
    Block.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    const auto SP = NdVar::reg(x86reg::RSP, 4);
    emitOp(Block, 0x1100, NdOp::INT_SUB, SP,
           {SP, NdVar::cst(Mutation == 5 ? 4 : 8, 4)});
    if (Mutation != 3)
      emitOp(Block, 0x1103, NdOp::STORE, {},
             {SP, Mutation == 4 ? NdVar::reg(x86reg::RBP, 4)
                                : NdVar::cst(Mutation == 1, 4)});
    emitOp(Block, 0x1106, NdOp::INT_ADD, NdVar::tmp(900, 4),
           {SP, NdVar::cst(4, 4)});
    emitOp(Block, 0x1106, NdOp::STORE, {},
           {NdVar::tmp(900, 4), NdVar::cst(Mutation == 2, 4)});
    emitOp(Block, 0x1109, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    RegistrationCalleeFrameContract Contract;
    Contract.CalleeKind = RegistrationCalleeFrameContract::Kind::RuntimeRethrow;
    Contract.Target = 0x2100;
    Contract.DoesNotReturn = true;
    const std::vector Contracts{Contract};
    const auto Result = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(Result.CallFrameEffectsComplete, Mutation == 0);
    EXPECT_EQ(Result.CallFrameEffects.size(), unsigned(Mutation == 0));
  }
}
} // namespace

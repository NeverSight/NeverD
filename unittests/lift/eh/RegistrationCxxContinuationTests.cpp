//===- RegistrationCxxContinuationTests.cpp - x86 C++ runtime resumes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

void overwriteSavedStackInCatch(LowFunc &F) {
  auto &Handler = F.Blocks[5];
  Handler.StartAddr = 0x17f7;
  F.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0].HandlerVA = 0x17f7;
  Handler.InstructionBoundaries.insert(Handler.InstructionBoundaries.begin(),
                                       {{0x17f7, 3}, {0x17fa, 3}, {0x17fd, 3}});
  LowBlock Write;
  emitOp(Write, 0x17f7, NdOp::INT_ADD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Write, 0x17fa, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-44), 4)});
  emitOp(Write, 0x17fd, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::reg(x86reg::RDX, 4)});
  Handler.Ops.insert(Handler.Ops.begin(), Write.Ops.begin(), Write.Ops.end());
  for (size_t I = 0; I < Handler.Ops.size(); ++I)
    Handler.Ops[I].Seq = I;
}

TEST(RegistrationState, CatchReturnRestoresTheRuntimeSavedStackSnapshot) {
  auto F = makeCxxCatchContinuation();
  overwriteSavedStackInCatch(F);
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.CxxContinuationsComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 1u);
  EXPECT_EQ(Result.CxxContinuations.front().SavedStackOffset, -28);
}

TEST(RegistrationState, ContinuationReadsTheRestoredSavedStackCell) {
  for (bool ScalarWrite : {false, true}) {
    auto F = makeCxxCatchContinuation();
    overwriteSavedStackInCatch(F);
    if (ScalarWrite)
      F.Blocks[5].Ops[2].Inputs[1] = NdVar::cst(7, 4);
    F.Blocks.resize(8);
    F.Blocks[6].Succs = {7};
    auto &Resume = F.Blocks[7];
    Resume.Id = 7;
    Resume.StartAddr = 0x1907;
    Resume.InstructionBoundaries = {{0x1907, 3}};
    Resume.EndAddr = 0x190a;
    Resume.Succs = {3};
    emitOp(Resume, 0x1907, NdOp::INT_ADD, NdVar::tmp(90, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Resume, 0x1907, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
           {NdVar::tmp(90, 4)});
    const auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    ASSERT_TRUE(Result.CxxContinuationsComplete);
    const auto Value =
        std::find_if(Result.FrameValues.begin(), Result.FrameValues.end(),
                     [&](const RegistrationFrameValue &Candidate) {
                       return Candidate.Address == 0x1907 &&
                              Candidate.OpSeq == Resume.Ops.back().Seq;
                     });
    ASSERT_NE(Value, Result.FrameValues.end());
    EXPECT_EQ(Value->EstablishedFrameOffset, -28);
  }
}

TEST(RegistrationState, CatchWriteCannotInventAnUnknownRuntimeSnapshot) {
  auto F = makeCxxCatchContinuation();
  F.Blocks[1].Ops[2].Inputs[1] = NdVar::cst(0, 4);
  overwriteSavedStackInCatch(F);
  const auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.CxxContinuationsComplete);
  EXPECT_TRUE(Result.CxxContinuations.empty());
}

TEST(RegistrationState, CxxCatchResumesWithTheRuntimeStackAndState) {
  const auto F = makeCxxCatchContinuation();
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.CallbackStatesComplete);
  EXPECT_TRUE(Result.CxxContinuationsComplete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_TRUE(Result.ChainOperationsComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 1u);
  EXPECT_EQ(
      Result.CxxContinuations[0],
      (RegistrationCxxContinuation{0, 0, 0x1805, 0x1806, 1, 0x1900, -28}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
  EXPECT_EQ(Result.Blocks[5].CxxMinimumTryLevel, 1);
  EXPECT_FALSE(Result.Blocks[6].CallbackOnly);
  EXPECT_EQ(Result.Blocks[6].Levels, (std::vector<int32_t>{1}));
}

TEST(RegistrationState,
     MissingCxxContinuationKeepsTheCandidateWithoutAuthority) {
  const auto Result =
      analyzeRegistrationStates(makeCxxCatchContinuation(false));
  ASSERT_EQ(Result.CxxContinuations.size(), 1u);
  EXPECT_EQ(Result.CxxContinuations[0].TargetVA, 0x1900u);
  EXPECT_FALSE(Result.CxxContinuationsComplete);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, CxxReturnsNeedExactContextStackAndDecodedReturn) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    auto F = makeCxxCatchContinuation();
    auto &Handler = F.Blocks[5];
    if (Mutation == 0)
      Handler.Ops[0].Inputs[0] = NdVar::cst(9, 4);
    if (Mutation == 1)
      Handler.InstructionBoundaries.back().Control =
          LowInstructionControl::TailCall;
    if (Mutation == 2)
      Handler.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 3)
      F.Blocks[1].Ops[2].Inputs[1] = NdVar::cst(0, 4);
    if (Mutation == 4)
      F.OrdinaryModuleAnalysisRoots.insert(Handler.StartAddr);
    if (Mutation == 5)
      Handler.Ops.back().Inputs[0].Size = 2;
    if (Mutation == 6)
      Handler.Ops[0].Opcode = NdOp::CALL;
    if (Mutation == 7)
      Handler.Ops.back().Seq = -1;
    if (Mutation == 8)
      Handler.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    const auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete) << Mutation;
    EXPECT_FALSE(Result.CxxContinuationsComplete) << Mutation;
    EXPECT_FALSE(Result.RegistrationLifetimeComplete) << Mutation;
    EXPECT_TRUE(Result.CxxContinuations.empty()) << Mutation;
  }
}

TEST(RegistrationState, NestedCxxCatchResumesTheEnclosingCatchContext) {
  auto F = makeCxxCatchContinuation();
  auto &EH = *F.ExceptionMetadata;
  auto &Cxx = *EH.Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap.resize(4, {-1, 0, CxxUnwindAction::ActionKind::None});
  Cxx.TryBlocks[0].CatchHigh = 3;
  auto Inner = Cxx.TryBlocks[0];
  Inner.TryLow = Inner.TryHigh = 2;
  Inner.CatchHigh = 3;
  Inner.Handlers[0].HandlerVA = 0x1850;
  Cxx.TryBlocks.push_back(Inner);
  F.Blocks.resize(12);
  F.Blocks[8] = F.Blocks[5];
  F.Blocks[8].Id = 8;
  F.Blocks[8].StartAddr = 0x1830;
  F.Blocks[8].EndAddr = 0x1836;
  for (auto &Op : F.Blocks[8].Ops)
    Op.Addr += 0x30;
  for (auto &Boundary : F.Blocks[8].InstructionBoundaries)
    Boundary.Address += 0x30;
  F.Blocks[9] = F.Blocks[8];
  F.Blocks[9].Id = 9;
  F.Blocks[9].StartAddr = 0x1850;
  F.Blocks[9].EndAddr = 0x1856;
  for (auto &Op : F.Blocks[9].Ops)
    Op.Addr += 0x20;
  for (auto &Boundary : F.Blocks[9].InstructionBoundaries)
    Boundary.Address += 0x20;
  F.Blocks[9].Ops[0].Inputs[0] = NdVar::cst(0x1820, 4);

  auto StateBlock = [&](unsigned Id, va_t Address, int32_t Level,
                        int Successor) {
    auto &Block = F.Blocks[Id];
    Block = LowBlock{};
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 7;
    Block.InstructionBoundaries = {{Address, 7}};
    Block.Succs = {Successor};
    addSlotStore(Block, Level);
    EH.Registration->TryLevelStores.push_back({Address, Address + 7, Level});
  };
  StateBlock(5, 0x1800, 2, 7);
  StateBlock(10, 0x1820, 1, 8);
  StateBlock(11, 0x1840, 1, 8);
  // The inner catch resumes with the outer catch's saved stack; the outer
  // return must still restore the parent snapshot captured before either.
  overwriteSavedStackInCatch(F);
  auto &Protected = F.Blocks[7];
  Protected.Id = 7;
  Protected.StartAddr = 0x1810;
  Protected.EndAddr = 0x1811;
  Protected.InstructionBoundaries = {{0x1810, 1}};
  Protected.Succs = {11};
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.CxxContinuationsComplete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 2u);
  EXPECT_EQ(Result.CxxContinuations[0].TargetVA, 0x1900u);
  EXPECT_EQ(Result.CxxContinuations[1].TargetVA, 0x1820u);
  EXPECT_EQ(Result.CxxContinuations[0].SavedStackOffset, -28);
  EXPECT_EQ(Result.CxxContinuations[1].SavedStackOffset, -44);
  EXPECT_TRUE(Result.Blocks[10].CallbackOnly);
  EXPECT_EQ(Result.Blocks[10].CxxMinimumTryLevel, 1);
  EXPECT_TRUE(Result.Blocks[9].CallbackOnly);
  EXPECT_EQ(Result.Blocks[9].CxxMinimumTryLevel, 3);
  EXPECT_EQ(Result.Blocks[7].CxxSearches,
            (std::vector<RegistrationCxxSearch>{{2, 1, 0}}));
  EXPECT_FALSE(Result.Blocks[6].CallbackOnly);
}

TEST(RegistrationState, CatchEntryUsesTheRuntimeCatchState) {
  LowFunc F = makeCxxCatchContinuation();
  F.ExceptionMetadata->Personality = ExceptionPersonality::CxxFrameHandlerX86;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 7u);
  EXPECT_EQ(Result.Blocks[5].Levels, (std::vector<int32_t>{1}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
}

} // namespace

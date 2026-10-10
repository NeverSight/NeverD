//===- RegistrationCallbackStateTests.cpp - x86 callback stack proof ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

LowFunc realignedCatch() {
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
  F.Blocks.resize(6);
  auto &Handler = F.Blocks[4];
  Handler.Id = 4;
  Handler.StartAddr = 0x1100;
  Handler.EndAddr = 0x1118;
  Handler.InstructionBoundaries = {
      {0x1100, 1}, {0x1101, 3}, {0x1104, 3}, {0x1107, 3}, {0x110a, 3},
      {0x110d, 3}, {0x1110, 5}, {0x1115, 1}, {0x1116, 1}, {0x1117, 1}};
  Handler.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto Base = NdVar::reg(x86reg::RSI, 4);
  const auto Tmp = NdVar::tmp(0, 4);
  // Runtime EBP and callback ESP are independent. Save the former on the
  // callback stack before recovering the source entry EBP through ESI.
  emit(Handler, 0x1100, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
  emit(Handler, 0x1100, NdOp::STORE, {}, {SP, FP});
  emit(Handler, 0x1101, NdOp::INT_ADD, Base,
       {FP, NdVar::cst(uint32_t(-60), 4)});
  emit(Handler, 0x1104, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(40, 4)});
  emit(Handler, 0x1104, NdOp::LOAD, FP, {Tmp});
  emit(Handler, 0x1107, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(44, 4)});
  emit(Handler, 0x1107, NdOp::STORE, {}, {Tmp, SP});
  // An entry-EBP argument load must stay separate from the runtime frame.
  emit(Handler, 0x110a, NdOp::INT_ADD, Tmp, {FP, NdVar::cst(8, 4)});
  emit(Handler, 0x110a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4), {Tmp});
  emit(Handler, 0x110d, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(36, 4)});
  emit(Handler, 0x110d, NdOp::STORE, {}, {Tmp, NdVar::cst(7, 4)});
  emit(Handler, 0x1110, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
       {NdVar::cst(0x1180, 4)});
  emit(Handler, 0x1115, NdOp::LOAD, FP, {SP});
  emit(Handler, 0x1116, NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
  emit(Handler, 0x1117, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});

  auto &Resume = F.Blocks[5];
  Resume.Id = 5;
  Resume.StartAddr = 0x1180;
  Resume.EndAddr = 0x1189;
  Resume.InstructionBoundaries = {{0x1180, 3}, {0x1183, 3}, {0x1186, 3}};
  Resume.Succs = {3};
  emit(Resume, 0x1180, NdOp::INT_ADD, Tmp, {FP, NdVar::cst(uint32_t(-16), 4)});
  emit(Resume, 0x1180, NdOp::LOAD, SP, {Tmp});
  emit(Resume, 0x1183, NdOp::INT_ADD, Base, {FP, NdVar::cst(uint32_t(-60), 4)});
  emit(Resume, 0x1186, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(40, 4)});
  emit(Resume, 0x1186, NdOp::LOAD, FP, {Tmp});
  return F;
}

LowFunc realignedCatchWithCall() {
  auto F = realignedCatch();
  auto &Handler = F.Blocks[4];
  for (auto &Op : Handler.Ops)
    if (Op.Addr >= 0x1110)
      Op.Addr += 8;
  for (auto &Boundary : Handler.InstructionBoundaries)
    if (Boundary.Address >= 0x1110)
      Boundary.Address += 8;
  Handler.EndAddr += 8;
  LowBlock Call;
  emit(Call, 0x1110, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
       {NdVar::reg(x86reg::RSI, 4), NdVar::cst(36, 4)});
  emit(Call, 0x1113, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
       {NdVar::cst(0x2100, 4)});
  const auto At = llvm::find_if(
      Handler.Ops, [](const auto &Op) { return Op.Addr == 0x1118; });
  Handler.Ops.insert(At, Call.Ops.begin(), Call.Ops.end());
  for (unsigned I = 0; I != Handler.Ops.size(); ++I)
    Handler.Ops[I].Seq = I;
  Handler.InstructionBoundaries.insert(
      Handler.InstructionBoundaries.begin() + 6, {{0x1110, 3}, {0x1113, 5}});
  Handler.InstructionBoundaries[7].Control = LowInstructionControl::Call;
  return F;
}

TEST(RegistrationCallback, BorrowsInitializedParentWhileUsingPrivateESP) {
  auto F = realignedCatchWithCall();
  RegistrationCalleeFrameContract Leaf;
  Leaf.Target = 0x2100;
  Leaf.ECXReads = Leaf.ECXWrites = {{0, 4}};
  const std::vector<RegistrationCalleeFrameContract> Callees{Leaf};
  const auto State = analyzeRegistrationStates(F, 0, 0, &Callees);
  ASSERT_TRUE(State.Complete);
  ASSERT_TRUE(State.CallFrameEffectsComplete);
  ASSERT_TRUE(State.CxxContinuationsComplete);
  ASSERT_EQ(State.CallFrameEffects.size(), 1u);
  EXPECT_EQ(State.CallFrameEffects[0].ECXFrameOffset, -24);
  EXPECT_EQ(State.CallFrameEffects[0].FrameReads,
            (std::vector<RegistrationObjectExtent>{{-24, -20}}));
  EXPECT_EQ(State.CxxContinuations[0].SavedStackOffset, -60);
  std::reverse(F.Blocks.begin(), F.Blocks.end());
  const auto Reordered = analyzeRegistrationStates(F, 0, 0, &Callees);
  ASSERT_TRUE(Reordered.Complete);
  EXPECT_EQ(Reordered.CxxContinuations, State.CxxContinuations);
  EXPECT_EQ(Reordered.CallFrameEffects, State.CallFrameEffects);
}

TEST(RegistrationCallback, RejectsInvalidParentBorrowsAndUnknownCalls) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = realignedCatchWithCall();
    RegistrationCalleeFrameContract Leaf;
    Leaf.Target = 0x2100;
    Leaf.ECXReads = Leaf.ECXWrites = {{0, 4}};
    if (Mutation == 0)
      Leaf.ECXWrites = {{4, 8}}; // The distinct saved entry EBP at -20.
    if (Mutation == 1)
      Leaf.ECXReads = {{-4, 0}}; // Uninitialized parent local.
    if (Mutation == 2)
      Leaf.Target += 1;
    if (Mutation == 3)
      for (auto &Op : F.Blocks[4].Ops)
        if (Op.Addr == 0x1110)
          Op.Inputs[0] = NdVar::reg(x86reg::RSP, 4);
    if (Mutation == 4)
      for (auto &Op : F.Blocks[4].Ops)
        if (Op.Addr == 0x1100 && Op.Opcode == NdOp::INT_SUB)
          Op.Inputs[1] = NdVar::cst(uint32_t(-4), 4);
    const std::vector<RegistrationCalleeFrameContract> Callees{Leaf};
    const auto State = analyzeRegistrationStates(F, 0, 0, &Callees);
    EXPECT_FALSE(State.CallFrameEffectsComplete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationCallback, ReleasedPrivateCellsCannotProveALaterReload) {
  auto F = realignedCatch();
  auto &Handler = F.Blocks[4];
  LowBlock Release;
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  emit(Release, 0x1115, NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
  emit(Release, 0x1115, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
  const auto At = llvm::find_if(
      Handler.Ops, [](const auto &Op) { return Op.Addr == 0x1115; });
  Handler.Ops.insert(At, Release.Ops.begin(), Release.Ops.end());
  for (unsigned I = 0; I != Handler.Ops.size(); ++I)
    Handler.Ops[I].Seq = I;
  const auto State = analyzeRegistrationStates(F);
  EXPECT_FALSE(State.CxxContinuationsComplete);
  EXPECT_FALSE(State.ChainOperationsComplete);
}

TEST(RegistrationCallback, RestoresTheRealignedParentThroughPrivateStack) {
  const auto State = analyzeRegistrationStates(realignedCatch());
  for (const auto &D : State.Diagnostics)
    SCOPED_TRACE(D);
  ASSERT_TRUE(State.Complete);
  EXPECT_TRUE(State.CallbackStatesComplete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  EXPECT_TRUE(State.ChainOperationsComplete);
  EXPECT_TRUE(State.ImageReadsComplete);
  ASSERT_TRUE(State.CxxContinuationsComplete);
  ASSERT_EQ(State.CxxContinuations.size(), 1u);
  EXPECT_EQ(State.CxxContinuations[0].TargetVA, 0x1180u);
  EXPECT_EQ(State.CxxContinuations[0].SavedStackOffset, -60);
  ASSERT_TRUE(State.IncomingFrameAccessesComplete);
  ASSERT_EQ(State.IncomingFrameAccesses.size(), 1u);
  EXPECT_EQ(State.IncomingFrameAccesses[0].Offset, 8);
  const auto Restored = llvm::find_if(
      State.FrameValues, [](const auto &V) { return V.Address == 0x1180; });
  ASSERT_NE(Restored, State.FrameValues.end());
  EXPECT_EQ(Restored->EstablishedFrameOffset, -16);
}

TEST(RegistrationCallback, RejectsUnbalancedOrAliasedCallbackFrames) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = realignedCatch();
    for (auto &Op : F.Blocks[4].Ops) {
      if (Mutation == 0 && Op.Addr == 0x1116)
        Op.Inputs[1] = NdVar::cst(8, 4);
      if (Mutation == 1 && Op.Addr == 0x1115)
        Op.Inputs[0] = NdVar::reg(x86reg::RSI, 4);
      if (Mutation == 2 && Op.Addr == 0x1100 && Op.Opcode == NdOp::INT_SUB)
        Op.Inputs[1] = NdVar::cst(0, 4);
      if (Mutation == 3 && Op.Addr == 0x1104 && Op.Opcode == NdOp::INT_ADD)
        Op.Inputs[1] = NdVar::cst(36, 4);
      if (Mutation == 4 && Op.Addr == 0x110d && Op.Opcode == NdOp::STORE)
        Op.Inputs[1] = NdVar::reg(x86reg::RSP, 4);
      if (Mutation == 5 && Op.Addr == 0x1104 && Op.Opcode == NdOp::INT_ADD)
        Op.Inputs[0] = NdVar::reg(x86reg::RSP, 4);
      if (Mutation == 7 && Op.Addr == 0x1100 && Op.Opcode == NdOp::STORE)
        Op.Opcode = NdOp::COPY;
      if (Mutation == 8 && Op.Addr == 0x1100 && Op.Opcode == NdOp::STORE)
        Op.Inputs[1].Size = 2;
      if (Mutation == 9 && Op.Addr == 0x1115)
        Op.Inputs[0] = NdVar::reg(x86reg::RDI, 4);
      if (Mutation == 6 && Op.Addr == 0x1107 && Op.Opcode == NdOp::INT_ADD)
        Op.Inputs[1] = NdVar::cst(40, 4);
    }
    const auto State = analyzeRegistrationStates(F);
    if (Mutation == 3) {
      // The catch can still return with a balanced stack, but its argument
      // read no longer has a proven source-frame or immutable-image owner.
      EXPECT_FALSE(State.ImageReadsComplete);
    } else {
      EXPECT_FALSE(State.CxxContinuationsComplete);
      EXPECT_FALSE(State.Complete);
      EXPECT_FALSE(State.ChainOperationsComplete);
    }
  }
}

} // namespace

//===- RegistrationCatchStackTests.cpp - Nested PE32 callback stacks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify nested callback snapshots independently of the parent frame.
//===----------------------------------------------------------------------===//
#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc nestedCallbackStacks(unsigned Mutation = 0) {
  auto F = makeRealignedRegistrationFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.CodeRange.End = 0x1200;
  auto &Cxx = *EH.Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap = {{-1, 0}, {-1, 0}, {1, 0}, {1, 0}};
  CxxTryBlock Outer;
  Outer.TryLow = Outer.TryHigh = 0;
  Outer.CatchHigh = 3;
  Outer.Handlers.emplace_back().HandlerVA = 0x1100;
  auto Inner = Outer;
  Inner.TryLow = Inner.TryHigh = 2;
  Inner.Handlers[0].HandlerVA = 0x1140;
  Cxx.TryBlocks = {Inner, Outer};
  F.Blocks.resize(10);
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto Base = NdVar::reg(x86reg::RSI, 4);
  const auto Value = NdVar::reg(x86reg::RAX, 4);
  const auto Tmp = NdVar::tmp(0, 4);
  auto Configure = [&](unsigned Id, va_t Begin) -> LowBlock & {
    auto &Block = F.Blocks[Id];
    Block.Id = Id;
    Block.StartAddr = Begin;
    return Block;
  };
  auto Prologue = [&](unsigned Id, va_t Begin) -> LowBlock & {
    auto &B = Configure(Id, Begin);
    B.InstructionBoundaries = {
        {Begin, 1}, {Begin + 1, 3}, {Begin + 4, 3}, {Begin + 7, 3}};
    B.EndAddr = Begin + 10;
    emit(B, Begin, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
    auto SavedFP = FP;
    if (Mutation == 2 && Id == 4)
      SavedFP.Size = 2;
    emit(B, Begin, NdOp::STORE, {}, {SP, SavedFP});
    emit(B, Begin + 1, NdOp::INT_ADD, Base, {FP, NdVar::cst(uint32_t(-60), 4)});
    emit(B, Begin + 4, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(40, 4)});
    emit(B, Begin + 4, NdOp::LOAD, FP, {Tmp});
    emit(B, Begin + 7, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(44, 4)});
    const auto Saved = Mutation == 1 && Id == 4 ? NdVar::cst(0, 4) : SP;
    emit(B, Begin + 7, NdOp::STORE, {}, {Tmp, Saved});
    return B;
  };
  auto Return = [&](LowBlock &B, va_t Address, va_t Target, bool Inner) {
    B.InstructionBoundaries.insert(
        B.InstructionBoundaries.end(),
        {{Address, 5}, {Address + 5, 1}, {Address + 6, 1}, {Address + 7, 1}});
    B.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    B.EndAddr = Address + 8;
    emit(B, Address, NdOp::COPY, Value, {NdVar::cst(Target, 4)});
    emit(B, Address + 5, NdOp::LOAD, FP, {SP});
    emit(B, Address + 6, NdOp::INT_ADD, SP,
         {SP, NdVar::cst(Mutation == 3 && Inner ? 8 : 4, 4)});
    emit(B, Address + 7, NdOp::RETURN, {}, {Value});
  };
  auto &OuterEntry = Prologue(4, 0x1100);
  OuterEntry.InstructionBoundaries.push_back({0x110a, 7});
  OuterEntry.EndAddr = 0x1111;
  OuterEntry.Succs = {8};
  emit(OuterEntry, 0x110a, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(56, 4)});
  emit(OuterEntry, 0x110a, NdOp::STORE, {}, {Tmp, NdVar::cst(2, 4)});
  EH.Registration->TryLevelStores.push_back({0x110a, 0x1111, 2});
  auto &Protected = Configure(8, 0x1111);
  Protected.InstructionBoundaries = {{0x1111, 3}};
  Protected.EndAddr = 0x1114;
  Protected.Succs = {6};
  emit(Protected, 0x1111, NdOp::INT_ADD, FP, {Base, NdVar::cst(60, 4)});
  auto &InnerEntry = Prologue(5, 0x1140);
  Return(InnerEntry, 0x114a, 0x1160, true);
  auto &OuterResume = Configure(6, 0x1160);
  OuterResume.InstructionBoundaries = {{0x1160, 7}};
  if (Mutation == 4)
    emit(OuterResume, 0x1160, NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
  emit(OuterResume, 0x1160, NdOp::INT_ADD, Tmp,
       {FP, NdVar::cst(uint32_t(-4), 4)});
  emit(OuterResume, 0x1160, NdOp::STORE, {}, {Tmp, NdVar::cst(1, 4)});
  EH.Registration->TryLevelStores.push_back({0x1160, 0x1167, 1});
  OuterResume.EndAddr = 0x1167;
  OuterResume.Succs = {9};
  Return(Configure(9, 0x1167), 0x1167, 0x1180, false);
  auto &ParentResume = Configure(7, 0x1180);
  ParentResume.EndAddr = 0x1189;
  ParentResume.InstructionBoundaries = {{0x1180, 3}, {0x1183, 3}, {0x1186, 3}};
  ParentResume.Succs = {3};
  emit(ParentResume, 0x1180, NdOp::INT_ADD, Tmp,
       {FP, NdVar::cst(uint32_t(-16), 4)});
  emit(ParentResume, 0x1180, NdOp::LOAD, SP, {Tmp});
  emit(ParentResume, 0x1183, NdOp::INT_ADD, Base,
       {FP, NdVar::cst(uint32_t(-60), 4)});
  emit(ParentResume, 0x1186, NdOp::INT_ADD, Tmp, {Base, NdVar::cst(40, 4)});
  emit(ParentResume, 0x1186, NdOp::LOAD, FP, {Tmp});
  return F;
}

TEST(RegistrationCatchStack, RestoresTheSuspendedCallbackBeforeItsParent) {
  for (bool Reversed : {false, true}) {
    auto F = nestedCallbackStacks();
    if (Reversed)
      std::reverse(F.Blocks.begin(), F.Blocks.end());
    const auto State = analyzeRegistrationStates(F);
    ASSERT_TRUE(State.Complete)
        << (State.Diagnostics.empty() ? "" : State.Diagnostics.front());
    ASSERT_TRUE(State.CxxContinuationsComplete);
    ASSERT_EQ(State.CxxContinuations.size(), 2u);
    const auto &Inner = State.CxxContinuations[0];
    const auto &Outer = State.CxxContinuations[1];
    EXPECT_EQ(Inner.SavedCallbackVA, 0x1100u);
    EXPECT_EQ(Inner.SavedStackOffset, -4);
    EXPECT_EQ(Inner.TargetVA, 0x1160u);
    EXPECT_EQ(Outer.SavedCallbackVA, 0u);
    EXPECT_EQ(Outer.SavedStackOffset, -60);
    EXPECT_EQ(Outer.TargetVA, 0x1180u);
  }
}

TEST(RegistrationCatchStack, RejectsMissingSnapshotsAndUnbalancedRestores) {
  for (unsigned Mutation = 1; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto State =
        analyzeRegistrationStates(nestedCallbackStacks(Mutation));
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.CxxContinuationsComplete);
    EXPECT_FALSE(State.RegistrationLifetimeComplete);
  }
}
} // namespace

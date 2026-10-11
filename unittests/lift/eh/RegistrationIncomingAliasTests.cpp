//===- RegistrationIncomingAliasTests.cpp - Incoming frame aliases -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve an incoming-frame spill only across disjoint proved EH writes.
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc makeIncomingAlias() {
  auto F = makeRealignedRegistrationFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.CodeRange.End = 0x1120;
  EH.Cxx->MaxState = 2;
  EH.Cxx->UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                       {-1, 0, CxxUnwindAction::ActionKind::None}};
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  Try.Handlers.emplace_back();
  Try.Handlers[0].HandlerVA = 0x1100;
  EH.Cxx->TryBlocks.push_back(Try);
  // Save the canonical incoming frame at runtime EBP - 28.
  emit(F.Blocks[0], 0x101d, NdOp::INT_ADD, NdVar::tmp(80, 4),
       {NdVar::reg(x86reg::RSI, 4), NdVar::cst(32, 4)});
  emit(F.Blocks[0], 0x101d, NdOp::STORE, {},
       {NdVar::tmp(80, 4), NdVar::reg(x86reg::RBP, 4)});
  F.Blocks.emplace_back();
  auto &Catch = F.Blocks.back();
  Catch.Id = 4;
  Catch.StartAddr = 0x1100;
  Catch.EndAddr = 0x1110;
  Catch.InstructionBoundaries = {{0x1100, 3}, {0x1103, 3}, {0x1106, 3},
                                 {0x1109, 3}, {0x110c, 3}, {0x110f, 1}};
  Catch.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  emit(Catch, 0x1100, NdOp::INT_ADD, NdVar::reg(x86reg::RAX, 4),
       {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
  emit(Catch, 0x1103, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
       {NdVar::reg(x86reg::RAX, 4)});
  emit(Catch, 0x1106, NdOp::INT_ADD, NdVar::reg(x86reg::RAX, 4),
       {NdVar::reg(x86reg::RAX, 4), NdVar::cst(8, 4)});
  emit(Catch, 0x1109, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
       {NdVar::reg(x86reg::RAX, 4)});
  // Resume a restore block before using the realigned local base again.
  emit(Catch, 0x110c, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
       {NdVar::cst(0x1110, 4)});
  emit(Catch, 0x110f, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});
  F.Blocks.emplace_back();
  auto &Resume = F.Blocks.back();
  Resume.Id = 5;
  Resume.StartAddr = 0x1110;
  Resume.EndAddr = 0x1116;
  Resume.Succs = {3};
  Resume.InstructionBoundaries = {{0x1110, 6}};
  emit(Resume, 0x1110, NdOp::INT_ADD, NdVar::reg(x86reg::RSI, 4),
       {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-60), 4)});
  return F;
}

TEST(RegistrationIncomingAlias, KeepsDisjointSpillAcrossUnwind) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeIncomingAlias();
    F.ExceptionMetadata->Cxx->UnwindMap[0] = {
        -1, 0x2000, CxxUnwindAction::ActionKind::Direct};
    std::vector<RegistrationCleanupFrameContract> Cleanups(1);
    auto &Cleanup = Cleanups[0];
    Cleanup.ActionState = 0;
    Cleanup.RelayTarget = 0x2000;
    Cleanup.Calls.emplace_back();
    Cleanup.Calls[0].ObjectFrameOffset = -32;
    Cleanup.Calls[0].Leaf.Target = 0x2100;
    Cleanup.Calls[0].Leaf.ECXWrites = {{0, 4}};
    if (Mutation == 1)
      Cleanup.Calls[0].Leaf.ECXWrites = {{4, 5}};
    if (Mutation == 2)
      Cleanup.Calls[0].Leaf.ECXWrites = {{7, 8}};
    if (Mutation == 3)
      Cleanups.clear();
    const std::vector<RegistrationCalleeFrameContract> Callees;
    const auto State = analyzeRegistrationStates(F, 0, 0, &Callees, &Cleanups);
    ASSERT_TRUE(State.Complete);
    EXPECT_EQ(State.IncomingFrameAccessesComplete, Mutation == 0);
    if (Mutation == 0) {
      ASSERT_EQ(State.IncomingFrameAccesses.size(), 1u);
      EXPECT_EQ(State.IncomingFrameAccesses[0].Address, 0x1109u);
      EXPECT_EQ(State.IncomingFrameAccesses[0].Offset, 8);
      EXPECT_EQ(State.IncomingFrameAccesses[0].Width, 4);
    }
  }
}

TEST(RegistrationIncomingAlias, CatchConstructionOverwritesTheSpill) {
  auto F = makeIncomingAlias();
  auto &Catch = F.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0];
  Catch.CatchObjectOffset = -28;
  Catch.TypeDescriptorVA = 0x3000;
  std::vector<RegistrationCalleeFrameContract> Callees(1);
  auto &Throw = Callees[0];
  Throw.Target = 0x2100;
  Throw.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
  Throw.DoesNotReturn = true;
  Throw.ThrownTypeVA = 0x3000;
  Throw.ThrownObjectSize = 4;
  const auto State = analyzeRegistrationStates(F, 0, 0, &Callees);
  EXPECT_TRUE(State.IncomingFrameAccesses.empty());
  EXPECT_FALSE(State.ImageReadsComplete);
}
} // namespace

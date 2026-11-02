//===- RegistrationRuntimeMemberStateTests.cpp - PE32 member receivers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc makeReferenceMemberCall(int32_t Offset = 0) {
  auto Function = makeCxxCatchContinuation();
  auto &Catch = Function.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0];
  Catch.TypeDescriptorVA = 0x3000;
  Catch.CatchObjectOffset = -24;
  Catch.Adjectives = 8;
  auto &Handler = Function.Blocks[5];
  for (auto &Op : Handler.Ops)
    Op.Addr += 10;
  for (auto &Boundary : Handler.InstructionBoundaries)
    Boundary.Address += 10;
  Handler.EndAddr += 10;
  LowBlock Call;
  emitOp(Call, 0x1800, NdOp::INT_ADD, NdVar::tmp(60, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Call, 0x1800, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(60, 4)});
  emitOp(Call, 0x1800, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RCX, 4), NdVar::cst(uint32_t(Offset), 4)});
  emitOp(Call, 0x1805, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x2200, 4)});
  Handler.Ops.insert(Handler.Ops.begin(), Call.Ops.begin(), Call.Ops.end());
  for (size_t I = 0; I != Handler.Ops.size(); ++I)
    Handler.Ops[I].Seq = I;
  Handler.InstructionBoundaries.insert(Handler.InstructionBoundaries.begin(),
                                       {{0x1800, 5}, {0x1805, 5}});
  Handler.InstructionBoundaries[1].Control = LowInstructionControl::Call;
  return Function;
}

std::vector<RegistrationCalleeFrameContract> memberContracts() {
  std::vector<RegistrationCalleeFrameContract> Contracts(2);
  auto &Throw = Contracts[0];
  Throw.Target = 0x2100;
  Throw.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
  Throw.DoesNotReturn = true;
  Throw.ThrownTypeVA = 0x3000;
  Throw.ThrownObjectSize = 12;
  auto &Member = Contracts[1];
  Member.Target = 0x2200;
  Member.HasIndependentScalarReturn = true;
  Member.ECXReads = Member.ECXWrites = {{0, 4}};
  return Contracts;
}

TEST(RegistrationRuntimeMemberState, KeepsTheLiveAllocationDistinctFromFrame) {
  for (int32_t Offset : {0, 4, 8}) {
    SCOPED_TRACE(Offset);
    auto Function = makeReferenceMemberCall(Offset);
    const auto Contracts = memberContracts();
    const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
    ASSERT_TRUE(States.Complete);
    ASSERT_TRUE(States.CallFrameEffectsComplete);
    ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
    ASSERT_EQ(States.CallFrameEffects.size(), 1u);
    const auto &Call = States.CallFrameEffects.front();
    EXPECT_FALSE(Call.ECXFrameOffset);
    ASSERT_TRUE(Call.ECXRuntimeObject);
    EXPECT_EQ(*Call.ECXRuntimeObject,
              (RegistrationRuntimeObjectBorrow{0, 0, Offset}));
    EXPECT_TRUE(Call.FrameReads.empty());
    EXPECT_TRUE(Call.FrameWrites.empty());
    std::reverse(Function.Blocks.begin(), Function.Blocks.end());
    const auto Reordered =
        analyzeRegistrationStates(Function, 0, 0, &Contracts);
    EXPECT_TRUE(Reordered.Complete);
    EXPECT_EQ(Reordered.CallFrameEffects, States.CallFrameEffects);
  }
}

TEST(RegistrationRuntimeMemberState, RejectsBoundsAndMissingObjectIdentity) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = makeReferenceMemberCall();
    auto Contracts = memberContracts();
    if (Mutation == 0)
      Contracts[1].ECXReads = {{-1, 4}};
    if (Mutation == 1)
      Contracts[1].ECXWrites = {{8, 13}};
    if (Mutation == 2)
      Contracts[0].ThrownObjectSize = 3;
    if (Mutation == 3)
      Function.Blocks[5].Ops[2].Inputs[1] = NdVar::cst(uint32_t(-1), 4);
    if (Mutation == 4)
      Function.Blocks[5].Ops[1].Output.Size = 2;
    if (Mutation == 5)
      Function.Blocks[5].Ops[2].Inputs[0] = NdVar::cst(0x3000, 4);
    if (Mutation == 6) {
      Contracts[1].HasIndependentScalarReturn = false;
      Contracts[1].CalleeKind =
          RegistrationCalleeFrameContract::Kind::Initializer;
      Contracts[1].ECXInitialized = Contracts[1].ECXWrites;
    }
    const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
    EXPECT_FALSE(States.CallFrameEffectsComplete);
    EXPECT_TRUE(States.CallFrameEffects.empty());
  }
}

TEST(RegistrationRuntimeMemberState, AnEndedCatchCannotAuthorizeABorrow) {
  auto Function = makeReferenceMemberCall();
  auto &Handler = Function.Blocks[5];
  // Save a genuine alias across catchret; the allocation ceases to be live.
  auto Save = Handler.Ops[2];
  Save.Opcode = NdOp::COPY;
  Save.Output = NdVar::reg(x86reg::RBX, 4);
  Save.NumInputs = 1;
  Save.Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  Handler.Ops.insert(Handler.Ops.begin() + 3, Save);
  for (size_t I = 0; I != Handler.Ops.size(); ++I)
    Handler.Ops[I].Seq = I;
  LowBlock Call;
  Call.Id = 7;
  Call.StartAddr = 0x1910;
  Call.EndAddr = 0x1918;
  Call.InstructionBoundaries = {{0x1910, 3}, {0x1913, 5}};
  Call.InstructionBoundaries[1].Control = LowInstructionControl::Call;
  emitOp(Call, 0x1910, NdOp::COPY, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RBX, 4)});
  emitOp(Call, 0x1913, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x2200, 4)});
  Function.Blocks[6].Succs = {7};
  Call.Succs = {3};
  Function.Blocks.push_back(std::move(Call));
  const auto Contracts = memberContracts();
  const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
  EXPECT_FALSE(States.CallFrameEffectsComplete);
}
} // namespace

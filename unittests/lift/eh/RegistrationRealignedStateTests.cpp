//===- RegistrationRealignedStateTests.cpp - PE32 frame coordinates -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

TEST(RegistrationRealignment, ReplaysTheEntryAndRuntimeFramesSeparately) {
  auto F = makeRealignedRegistrationFrame();
  auto State = analyzeRegistrationStates(F);
  for (const auto &D : State.Diagnostics)
    SCOPED_TRACE(D);
  ASSERT_TRUE(State.Complete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  EXPECT_TRUE(State.ChainOperationsComplete);
  ASSERT_EQ(State.Blocks.size(), 4u);
  EXPECT_EQ(State.Blocks.back().Levels, (std::vector<int32_t>{0}));
  EXPECT_TRUE(State.IncomingFrameAccessesComplete);
  EXPECT_TRUE(State.IncomingFrameAccesses.empty());
  const auto EntryFP = llvm::find_if(State.FrameValues, [](const auto &Value) {
    return Value.Address == 0x1001;
  });
  ASSERT_NE(EntryFP, State.FrameValues.end());
  EXPECT_FALSE(EntryFP->EstablishedFrameOffset);
}

TEST(RegistrationRealignment, RejectsChangedAllocationAlignmentAndRoot) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeRealignedRegistrationFrame();
    for (auto &Op : F.Blocks.front().Ops) {
      if (Mutation == 0 && Op.Opcode == NdOp::INT_AND && Op.Addr == 0x1006)
        Op.Inputs[1] = NdVar::cst(0xfffffff8, 4);
      if (Mutation == 1 && Op.Opcode == NdOp::INT_SUB && Op.Addr == 0x1009)
        Op.Inputs[1] = NdVar::cst(60, 4);
      if (Mutation == 2 && Op.Opcode == NdOp::COPY && Op.Addr == 0x100f)
        Op.Inputs[0] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 3 && Op.Opcode == NdOp::STORE && Op.Addr == 0x1011)
        Op.Inputs[1] = NdVar::reg(x86reg::RSI, 4);
      if (Mutation == 4 && Op.Opcode == NdOp::STORE && Op.Addr == 0x1017)
        Op.Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 5 && Op.Opcode == NdOp::STORE && Op.Addr == 0x101d)
        Op.Inputs[1] = NdVar::cst(0, 4);
    }
    const auto State = analyzeRegistrationStates(F);
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, RequiresDecodedEntryAndPublicationBoundaries) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeRealignedRegistrationFrame();
    if (Mutation == 0)
      F.Blocks[0].InstructionBoundaries[5].Size = 2;
    if (Mutation == 1)
      F.Blocks[0].InstructionBoundaries[5].Control =
          LowInstructionControl::Call;
    if (Mutation == 2)
      F.Blocks[1].InstructionBoundaries.back().Size = 7;
    if (Mutation == 3)
      F.ExceptionMetadata->Registration->RealignedFrame->BaseOffset = -64;
    const auto State = analyzeRegistrationStates(F);
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, EntryAddressCannotAliasTheAlignedStateSlot) {
  auto F = makeRealignedRegistrationFrame();
  for (auto &Op : F.Blocks[2].Ops)
    if (Op.Opcode == NdOp::INT_ADD)
      Op.Inputs[0] = NdVar::reg(x86reg::RBP, 4);
  const auto State = analyzeRegistrationStates(F);
  EXPECT_FALSE(State.Complete);
  EXPECT_FALSE(State.ChainOperationsComplete);
}

TEST(RegistrationRealignment, RejectsEntryLoadsBelowTheSavedRegisterArea) {
  auto F = makeRealignedRegistrationFrame();
  auto &Exit = F.Blocks.back();
  emit(Exit, 0x105a, NdOp::INT_ADD, NdVar::tmp(10, 4),
       {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emit(Exit, 0x105a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
       {NdVar::tmp(10, 4)});
  const auto State = analyzeRegistrationStates(F);
  EXPECT_FALSE(State.Complete);
  EXPECT_FALSE(State.IncomingFrameAccessesComplete);
  EXPECT_FALSE(State.ImageReadsComplete);
}

TEST(RegistrationRealignment, RejectsAlignedAccessesIntoTheEntryFrame) {
  for (unsigned Offset : {62u, 64u}) {
    SCOPED_TRACE(Offset);
    for (bool Store : {false, true}) {
      SCOPED_TRACE(Store);
      auto F = makeRealignedRegistrationFrame();
      auto &Exit = F.Blocks.back();
      emit(Exit, 0x105a, NdOp::INT_ADD, NdVar::tmp(10, 4),
           {NdVar::reg(x86reg::RSI, 4), NdVar::cst(Offset, 4)});
      if (Store)
        emit(Exit, 0x105a, NdOp::STORE, {},
             {NdVar::tmp(10, 4), NdVar::cst(0, 4)});
      else
        emit(Exit, 0x105a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
             {NdVar::tmp(10, 4)});
      const auto State = analyzeRegistrationStates(F);
      EXPECT_FALSE(State.Complete);
      EXPECT_FALSE(State.IncomingFrameAccessesComplete);
      EXPECT_FALSE(State.ImageReadsComplete);
    }
  }
}

} // namespace

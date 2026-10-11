//===- RegistrationCxxSearchTests.cpp - PE32 secondary catch search -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc makeSecondaryCatchSearch(bool ChangeSavedStack = false) {
  auto F = makeCxxCatchContinuation();
  auto &EH = *F.ExceptionMetadata;
  auto &Cxx = *EH.Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {0, 0}, {-1, 0}};
  Cxx.TryBlocks[0].TryLow = Cxx.TryBlocks[0].TryHigh = 1;
  Cxx.TryBlocks[0].CatchHigh = 2;
  auto Outer = Cxx.TryBlocks[0];
  Outer.TryLow = 0;
  Outer.TryHigh = 2;
  Outer.CatchHigh = 3;
  Outer.Handlers[0].HandlerVA = 0x1850;
  Cxx.TryBlocks.push_back(Outer);
  EH.Registration->TryLevelStores[0].Level = 1;
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
  F.Blocks.push_back(F.Blocks[5]);
  auto &Handler = F.Blocks.back();
  Handler.Id = 7;
  Handler.StartAddr += 0x50;
  Handler.EndAddr += 0x50;
  for (auto &Op : Handler.Ops)
    Op.Addr += 0x50;
  for (auto &Boundary : Handler.InstructionBoundaries)
    Boundary.Address += 0x50;
  if (ChangeSavedStack) {
    F.Blocks.push_back(LowBlock{});
    auto &Write = F.Blocks.back();
    Write.Id = 8;
    Write.StartAddr = 0x17f0;
    Write.EndAddr = 0x17f9;
    Write.InstructionBoundaries = {{0x17f0, 3}, {0x17f3, 3}, {0x17f6, 3}};
    Write.Succs = {5};
    Cxx.TryBlocks[0].Handlers[0].HandlerVA = Write.StartAddr;
    emitOp(Write, 0x17f0, NdOp::INT_ADD, NdVar::reg(x86reg::RDI, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Write, 0x17f3, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-44), 4)});
    emitOp(Write, 0x17f6, NdOp::STORE, {},
           {NdVar::reg(x86reg::RDI, 4), NdVar::reg(x86reg::RDX, 4)});
  }
  return F;
}

TEST(RegistrationState, SecondaryCatchSearchRestoresTheExitedGuardSnapshot) {
  // Both the parent and the inner catch can select the outer catch. Windows
  // discards the inner callback's SavedESP write when exiting its guard.
  const auto Result = analyzeRegistrationStates(makeSecondaryCatchSearch(true));
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.CxxContinuationsComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 2u);
  EXPECT_EQ(Result.CxxContinuations[1].TryIndex, 1u);
  EXPECT_EQ(Result.CxxContinuations[1].SavedStackOffset, -28);
  EXPECT_EQ(Result.Blocks[5].CxxSearches,
            (std::vector<RegistrationCxxSearch>{{2, 1, 1}}));
}

TEST(RegistrationState, SecondaryCatchSearchExitsTheInnerInvocation) {
  const auto Result = analyzeRegistrationStates(makeSecondaryCatchSearch());
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.CxxContinuationsComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 2u);
  EXPECT_EQ(Result.CxxContinuations[1].TryIndex, 1u);
  EXPECT_EQ(Result.CxxContinuations[1].SavedStackOffset, -28);
  EXPECT_TRUE(Result.Blocks[7].CallbackOnly);
  EXPECT_EQ(Result.Blocks[7].CxxMinimumTryLevel, 3);
  EXPECT_FALSE(Result.Blocks[6].CallbackOnly);
  EXPECT_EQ(Result.Blocks[5].CxxSearches,
            (std::vector<RegistrationCxxSearch>{{2, 1, 1}}));
  EXPECT_TRUE(Result.Blocks[7].CxxSearches.empty());
}

TEST(RegistrationState, SecondarySearchCannotRebuildAMissingGuardSnapshot) {
  auto F = makeSecondaryCatchSearch(true);
  F.Blocks[1].Ops[2].Inputs[1] = NdVar::cst(0, 4);
  const auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.CxxContinuationsComplete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
}

TEST(RegistrationState, SecondarySearchDoesNotEnterAnUnprotectedPeer) {
  auto F = makeSecondaryCatchSearch();
  F.ExceptionMetadata->Cxx->TryBlocks[1].TryHigh = 0;
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[5].CxxSearches.empty());
  EXPECT_FALSE(Result.Blocks[7].Reached);
}

TEST(RegistrationState, SecondarySearchSurvivesBlockAndSuccessorReordering) {
  auto Original = makeSecondaryCatchSearch(true);
  // An ordinary backedge tests ordering without inventing a runtime entry.
  Original.Blocks[2].Succs.push_back(2);
  const auto Expected = analyzeRegistrationStates(Original);
  ASSERT_TRUE(Expected.Complete);
  ASSERT_EQ(Expected.CxxContinuations.size(), 2u);
  EXPECT_EQ(Expected.CxxContinuations[1].SavedStackOffset, -28);
  for (bool Reverse : {false, true}) {
    auto Function = Original;
    if (Reverse) {
      std::reverse(Function.Blocks.begin(), Function.Blocks.end());
      for (auto &Block : Function.Blocks)
        std::reverse(Block.Succs.begin(), Block.Succs.end());
    }
    const auto Result = analyzeRegistrationStates(Function);
    ASSERT_TRUE(Result.Complete);
    EXPECT_TRUE(Result.CxxContinuationsComplete);
    EXPECT_TRUE(Result.RegistrationLifetimeComplete);
    EXPECT_EQ(Result.CxxContinuations, Expected.CxxContinuations);
    for (const auto &Block : Result.Blocks) {
      const auto Found = llvm::find_if(Expected.Blocks, [&](const auto &Other) {
        return Other.BlockId == Block.BlockId;
      });
      ASSERT_NE(Found, Expected.Blocks.end());
      EXPECT_EQ(Block.Levels, Found->Levels);
      EXPECT_EQ(Block.CxxSearches, Found->CxxSearches);
      EXPECT_EQ(Block.CallbackOnly, Found->CallbackOnly);
      EXPECT_EQ(Block.Unknown, Found->Unknown);
    }
  }
}
} // namespace

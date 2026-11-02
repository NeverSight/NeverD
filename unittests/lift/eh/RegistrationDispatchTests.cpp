//===- RegistrationDispatchTests.cpp - Bounded SEH root joins ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks shared callback roots without discarding any reaching state.

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <numeric>

using namespace neverd;
using namespace neverd::registration_test;

TEST(RegistrationDispatch, ManyDistinctFramesStayWithinDefaultBudget) {
  for (bool ReverseBlocks : {false, true}) {
    SCOPED_TRACE(ReverseBlocks);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x1800, true}};
    constexpr int Count = 1024;
    F.ExceptionMetadata->CodeRange.End = 0x7000;
    F.Blocks[1].Succs = {4};
    // Each active block has the same registers and a distinct parent cell.
    // Comparing each snapshot with every earlier snapshot is quadratic even
    // though this function has only one finally and one active TryLevel.
    for (int I = 0; I != Count; ++I) {
      LowBlock Body;
      Body.Id = I + 4;
      Body.StartAddr = 0x2000 + 0x10 * I;
      Body.EndAddr = Body.StartAddr + 7;
      Body.Succs = {I + 1 == Count ? 3 : I + 5};
      emitOp(Body, Body.StartAddr, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
      emitOp(Body, Body.StartAddr, NdOp::STORE, {},
             {NdVar::tmp(0, 4), NdVar::cst(I, 4)});
      Body.InstructionBoundaries = {{Body.StartAddr, 7, 0, 2}};
      F.Blocks.push_back(std::move(Body));
    }
    LowBlock Callback;
    Callback.Id = Count + 4;
    Callback.StartAddr = 0x1800;
    Callback.EndAddr = 0x1801;
    emitOp(Callback, 0x1800, NdOp::RETURN, {}, {});
    Callback.InstructionBoundaries = {{0x1800, 1, 0, 1}};
    Callback.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    F.Blocks.push_back(std::move(Callback));
    if (ReverseBlocks)
      std::reverse(F.Blocks.begin(), F.Blocks.end());
    const auto A = analyzeRegistrationStates(F);
    for (const auto &Diagnostic : A.Diagnostics)
      ADD_FAILURE() << Diagnostic;
    ASSERT_TRUE(A.Complete);
    EXPECT_TRUE(A.CallbackStatesComplete);
    const auto Root =
        std::find_if(A.Blocks.begin(), A.Blocks.end(),
                     [](const auto &B) { return B.Range.Begin == 0x1800; });
    ASSERT_NE(Root, A.Blocks.end());
    EXPECT_TRUE(Root->CallbackOnly);
    EXPECT_FALSE(Root->Unknown);
    EXPECT_EQ(Root->levels(), std::vector<int32_t>{-1});
  }
}

TEST(RegistrationDispatch, SharedFinallyRetainsEveryLevelWithinDefaultBudget) {
  for (bool ReverseBlocks : {false, true}) {
    SCOPED_TRACE(ReverseBlocks);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    constexpr int32_t Count = 1024;
    Chain.Scopes.resize(Count);
    for (int32_t I = 0; I != Count; ++I)
      Chain.Scopes[I] = {I - 1, 0, 0x1800, true};
    Chain.TryLevelStores.front().Level = Count - 1;
    F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(Count - 1, 4);
    LowBlock Callback;
    Callback.Id = 4;
    Callback.StartAddr = 0x1800;
    Callback.EndAddr = 0x1801;
    emitOp(Callback, 0x1800, NdOp::RETURN, {}, {});
    Callback.InstructionBoundaries = {{0x1800, 1, 0, 1}};
    Callback.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    F.Blocks.push_back(std::move(Callback));
    if (ReverseBlocks)
      std::reverse(F.Blocks.begin(), F.Blocks.end());
    const auto A = analyzeRegistrationStates(F);
    for (const auto &Diagnostic : A.Diagnostics)
      ADD_FAILURE() << Diagnostic;
    ASSERT_TRUE(A.Complete);
    EXPECT_TRUE(A.CallbackStatesComplete);
    const auto Root =
        std::find_if(A.Blocks.begin(), A.Blocks.end(),
                     [](const auto &B) { return B.BlockId == 4; });
    ASSERT_NE(Root, A.Blocks.end());
    EXPECT_TRUE(Root->CallbackOnly);
    EXPECT_TRUE(Root->CanDispatch);
    EXPECT_FALSE(Root->Unknown);
    std::vector<int32_t> Expected(Count);
    std::iota(Expected.begin(), Expected.end(), -1);
    EXPECT_EQ(Root->levels(), Expected);
  }
}

TEST(RegistrationDispatch, DistinctFrameSnapshotsStillContributeToSharedRoots) {
  for (bool Agree : {false, true}) {
    for (bool ReverseBlocks : {false, true}) {
      SCOPED_TRACE(Agree);
      SCOPED_TRACE(ReverseBlocks);
      auto F = makeBranchingFrame();
      auto &Chain = *F.ExceptionMetadata->Registration;
      Chain.Scopes = {{-1, 0, 0x1800, true}};
      Chain.TryLevelStores[1].Level = 0;
      F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(0, 4);
      for (unsigned I = 1; I != 3; ++I) {
        auto &Arm = F.Blocks[I];
        emitOp(Arm, Arm.StartAddr, NdOp::INT_ADD, NdVar::tmp(10, 4),
               {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
        emitOp(Arm, Arm.StartAddr, NdOp::INT_ADD, NdVar::tmp(11, 4),
               {NdVar::reg(x86reg::RBP, 4),
                NdVar::cst(uint32_t(I == 1 || Agree ? -40 : -44), 4)});
        emitOp(Arm, Arm.StartAddr, NdOp::STORE, {},
               {NdVar::tmp(10, 4), NdVar::tmp(11, 4)});
      }
      F.Blocks[2].Succs = {5};
      LowBlock Other = F.Blocks[3];
      Other.Id = 5;
      Other.StartAddr = 0x1040;
      Other.EndAddr = 0x1047;
      Other.InstructionBoundaries.front().Address = Other.StartAddr;
      F.Blocks.push_back(std::move(Other));
      LowBlock Callback;
      Callback.Id = 4;
      Callback.StartAddr = 0x1800;
      Callback.EndAddr = 0x1804;
      emitOp(Callback, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
      emitOp(Callback, 0x1800, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
             {NdVar::tmp(0, 4)});
      emitOp(Callback, 0x1803, NdOp::RETURN, {}, {});
      Callback.InstructionBoundaries = {{0x1800, 3, 0, 2}, {0x1803, 1, 2, 1}};
      Callback.InstructionBoundaries.back().Control =
          LowInstructionControl::Return;
      F.Blocks.push_back(std::move(Callback));
      if (ReverseBlocks)
        std::reverse(F.Blocks.begin(), F.Blocks.end());
      const auto A = analyzeRegistrationStates(F);
      ASSERT_TRUE(A.Complete);
      ASSERT_TRUE(A.CallbackStatesComplete);
      const auto Load = std::find_if(
          A.FrameValues.begin(), A.FrameValues.end(), [](const auto &Value) {
            return Value.Address == 0x1800 && Value.OpSeq == 1;
          });
      ASSERT_NE(Load, A.FrameValues.end());
      EXPECT_EQ(Load->EstablishedFrameOffset,
                Agree ? std::optional<int32_t>{-40} : std::nullopt);
    }
  }
}

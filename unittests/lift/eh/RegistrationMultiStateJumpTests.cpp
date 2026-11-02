//===- RegistrationMultiStateJumpTests.cpp - Correlated jump bounds -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks source captures reached with different live SEH states.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

TEST(RegistrationMultiStateJump, KeepsCaptureBoundsAcrossParentJoins) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeJumpFrame(Mutation == 1 ? 1 : 0);
    F.Blocks.resize(10);
    F.Blocks[1].Succs = {7, 8};
    auto &Chain = *F.ExceptionMetadata->Registration;
    for (unsigned I = 7; I != 9; ++I) {
      auto &Block = F.Blocks[I];
      Block.Id = I;
      Block.StartAddr = 0x1090 + (I - 7) * 0x10;
      Block.EndAddr = Block.StartAddr + 7;
      Block.Succs = {2};
      addSlotStore(Block, I == 7 ? -1 : 0);
      for (unsigned J = 0; J != Block.Ops.size(); ++J)
        Block.Ops[J].Seq = J;
      Block.InstructionBoundaries = {{Block.StartAddr, 7, 0, 2}};
      Chain.TryLevelStores.push_back(
          {Block.StartAddr, Block.EndAddr, I == 7 ? -1 : 0});
    }
    // Both paths activate the same state after capturing. Joining only on the
    // current state would discard the distinct saved buffer bounds here.
    F.Blocks[3].Succs = {9, 5};
    auto &Activate = F.Blocks[9];
    Activate.Id = 9;
    Activate.StartAddr = 0x1150;
    Activate.EndAddr = 0x1157;
    Activate.Succs = {4};
    addSlotStore(Activate, 0);
    for (unsigned J = 0; J != Activate.Ops.size(); ++J)
      Activate.Ops[J].Seq = J;
    Activate.InstructionBoundaries = {{Activate.StartAddr, 7, 0, 2}};
    Chain.TryLevelStores.push_back({Activate.StartAddr, Activate.EndAddr, 0});
    if (Mutation == 3)
      F.Blocks[8].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
    if (Mutation == 4)
      std::reverse(F.Blocks.begin(), F.Blocks.end());

    SEHJumpRuntime Runtime;
    Runtime.Capture = [&](const LowOp &Op, const FrameState &Frame, bool,
                          size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      const auto Level = Frame.load(-4, 4).Constant;
      if (!Level)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.Level = int32_t(*Level);
      Capture.BufferOffset = Mutation == 2 && !*Level ? -140 : -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != std::size(Registers); ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x111a;
    };
    const auto Result =
        RegistrationStateSolver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, &Runtime)
            .run();
    if (Mutation != 0 && Mutation != 4) {
      EXPECT_FALSE(Result.Complete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      continue;
    }
    for (const auto &Diagnostic : Result.Diagnostics)
      ADD_FAILURE() << Diagnostic;
    ASSERT_TRUE(Result.Complete);
    ASSERT_EQ(Result.SEHJumpCaptures.size(), 1u);
    EXPECT_EQ(Result.SEHJumpCaptures.front().Levels,
              (std::vector<int32_t>{-1, 0}));
    ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
    EXPECT_EQ(Result.SEHJumpResumptions.front().StateRestores,
              (std::vector<RegistrationSEHJumpState>{{0, -1, -1}, {0, 0, 0}}));
  }
}

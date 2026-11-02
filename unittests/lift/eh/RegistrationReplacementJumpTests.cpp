//===- RegistrationReplacementJumpTests.cpp - Live capture transfers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks destination lifetimes before and after a cleanup replaces a jump.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace {
void moveBlock(LowBlock &Block, va_t Address) {
  const va_t Delta = Address - Block.StartAddr;
  Block.StartAddr += Delta;
  Block.EndAddr += Delta;
  for (auto &Op : Block.Ops)
    Op.Addr += Delta;
  for (auto &Boundary : Block.InstructionBoundaries)
    Boundary.Address += Delta;
}

LowFunc replacementFrame(unsigned Protocol, unsigned Mutation) {
  auto F = makeJumpFrame();
  if (Protocol)
    initializeEH4JumpFrame(F, Protocol == 2);
  F.Blocks.resize(14);
  auto &Chain = *F.ExceptionMetadata->Registration;
  const int32_t Outside = Protocol ? -2 : -1;
  Chain.Scopes = {{Outside, 0, 0x1900, true}, {0, 0, 0x1800, true}};
  // The captures have disjoint buffers, different ESP and different bounds.
  auto &Setup = F.Blocks[1];
  Setup.Ops.front().Inputs[1].Offset += 96;
  emitOp(Setup, 0x1088, NdOp::COPY, NdVar::reg(x86reg::RBX, 4),
         {NdVar::cst(6, 4)});
  Setup.InstructionBoundaries.push_back({0x1088, 5, 4, 1});
  Setup.EndAddr = 0x108d;
  F.Blocks[3].Succs = {7, 5};
  for (unsigned I = 7; I != F.Blocks.size(); ++I)
    F.Blocks[I].Id = I;
  auto &SecondSetup = F.Blocks[7];
  SecondSetup.StartAddr = 0x1160;
  SecondSetup.EndAddr = 0x116f;
  SecondSetup.Succs = {8};
  addSlotStore(SecondSetup, 0);
  emitOp(SecondSetup, 0x1167, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(32, 4)});
  emitOp(SecondSetup, 0x116a, NdOp::COPY, NdVar::reg(x86reg::RBX, 4),
         {NdVar::cst(8, 4)});
  SecondSetup.InstructionBoundaries = {
      {0x1160, 7, 0, 2}, {0x1167, 3, 2, 1}, {0x116a, 5, 3, 1}};
  if (Mutation == 2) {
    emitOp(SecondSetup, 0x116f, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-143), 4)});
    emitOp(SecondSetup, 0x116f, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
    SecondSetup.InstructionBoundaries.push_back({0x116f, 7, 4, 2});
    SecondSetup.EndAddr = 0x1176;
  }
  auto &SecondCapture = F.Blocks[8];
  SecondCapture = F.Blocks[2];
  SecondCapture.Id = 8;
  SecondCapture.Succs = {9};
  moveBlock(SecondCapture, 0x1180);
  auto &SecondResume = F.Blocks[9];
  SecondResume.StartAddr = 0x1185;
  SecondResume.EndAddr = 0x1188;
  SecondResume.Succs = {10};
  emitOp(SecondResume, 0x1185, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  SecondResume.InstructionBoundaries = {{0x1185, 3, 0, 1}};
  auto &Activate = F.Blocks[10];
  Activate.StartAddr = 0x1190;
  Activate.EndAddr = 0x1197;
  Activate.Succs = {4};
  addSlotStore(Activate, 1);
  Activate.InstructionBoundaries = {{0x1190, 7, 0, 2}};
  Chain.TryLevelStores.push_back({0x1160, 0x1167, 0});
  Chain.TryLevelStores.push_back({0x1190, 0x1197, 1});
  auto &Cleanup = F.Blocks[11];
  Cleanup = F.Blocks[4];
  Cleanup.Id = 11;
  moveBlock(Cleanup, 0x1800);
  Cleanup.Ops[4].Inputs[1] =
      Mutation == 8 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(43, 4);
  if (Mutation == 1)
    Cleanup.Ops[1].Inputs[1] = NdVar::cst(uint32_t(-148), 4);
  if (Mutation == 9)
    Cleanup.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
  auto &Jump = F.Blocks[4];
  moveBlock(Jump, 0x1200);
  Jump.Ops[1].Inputs[1] = NdVar::cst(uint32_t(Mutation == 7 ? -160 : -240), 4);
  auto &Outer = F.Blocks[12];
  Outer.StartAddr = 0x1900;
  va_t Return = 0x1900;
  if (Mutation >= 3 && Mutation <= 5) {
    // Even writes to uninitialized padding in either 64-byte buffer revoke
    // its lifetime. A disjoint scalar store is the positive control.
    const int32_t Offset = Mutation == 3 ? -104 : Mutation == 4 ? -180 : -72;
    emitOp(Outer, Return, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
    emitOp(Outer, Return, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
    Outer.InstructionBoundaries.push_back({Return, 7, 0, 2});
    Return += 7;
  }
  const size_t First = Outer.Ops.size();
  emitOp(Outer, Return, NdOp::RETURN, {}, {});
  Outer.EndAddr = Return + 1;
  Outer.InstructionBoundaries.push_back({Return, 1, First, 1});
  Outer.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  Outer.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  // Keep the capture's conditional branch tied to its physical fallthrough.
  // Otherwise a resumed nonzero value would also reach the second capture.
  moveBlock(SecondSetup, 0x110f);
  auto &SecondArguments = F.Blocks[13];
  SecondArguments.StartAddr = SecondSetup.StartAddr + 7;
  SecondArguments.EndAddr = SecondSetup.EndAddr;
  SecondArguments.Succs = SecondSetup.Succs;
  SecondArguments.Ops.assign(SecondSetup.Ops.begin() + 2,
                             SecondSetup.Ops.end());
  SecondArguments.InstructionBoundaries.assign(
      SecondSetup.InstructionBoundaries.begin() + 1,
      SecondSetup.InstructionBoundaries.end());
  for (auto &Boundary : SecondArguments.InstructionBoundaries)
    Boundary.FirstOp -= 2;
  SecondSetup.EndAddr = SecondArguments.StartAddr;
  SecondSetup.Ops.resize(2);
  SecondSetup.InstructionBoundaries.resize(1);
  SecondSetup.Succs = {13};
  moveBlock(F.Blocks[5], 0x1220);
  F.Blocks[3].Ops.back().Inputs[0] = NdVar::cst(0x1220, 8);
  for (auto &Store : Chain.TryLevelStores) {
    if (Store.StoreVA == 0x1160) {
      Store.StoreVA = 0x110f;
      Store.EndVA = 0x1116;
    } else if (Store.StoreVA == 0x1120) {
      Store.StoreVA = 0x1220;
      Store.EndVA = 0x1227;
    }
  }
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  if (Mutation == 6)
    std::reverse(F.Blocks.begin(), F.Blocks.end());
  return F;
}
} // namespace

TEST(RegistrationReplacementJump, BothBuffersSurviveTheWholeUnwind) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(Mutation);
      const auto F = replacementFrame(Protocol, Mutation);
      SEHJumpRuntime Runtime;
      Runtime.Capture = [&](const LowOp &Op, const FrameState &Frame, bool,
                            size_t &) -> std::optional<SEHJumpCapture> {
        if (Op.Addr != 0x1100 && Op.Addr != 0x1180)
          return std::nullopt;
        const bool Second = Op.Addr == 0x1180;
        SEHJumpCapture Capture;
        Capture.Call = Op.Addr;
        Capture.ReturnPC = Op.Addr + 5;
        Capture.BufferOffset = Second ? (Mutation == 7 ? -160 : -240) : -144;
        Capture.StackOffset = Second ? -304 : -288;
        Capture.Level = Second ? 0 : Protocol ? -2 : -1;
        EXPECT_EQ(
            Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset,
            Capture.StackOffset);
        Capture.UnwindTarget = 0x3300;
        if (Protocol) {
          Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
          Capture.SecurityCookieVA = 0x4000;
        }
        constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                          x86reg::RSI};
        for (unsigned I = 0; I != std::size(Registers); ++I)
          Capture.Nonvolatile[I] =
              Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
        return Capture;
      };
      Runtime.Resume = [](const LowOp &Op, size_t &) {
        return Op.Addr == 0x120b || Op.Addr == 0x180b;
      };
      std::vector<RegistrationSEHControlContract> Controls;
      for (const auto &Block : F.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::RETURN)
            Controls.push_back(
                {RegistrationSEHControlContract::Kind::NearReturn, Op.Addr,
                 Block.EndAddr, 0});
      const auto Result =
          RegistrationStateSolver(F, Protocol ? 0x4000 : 0, 0, nullptr, nullptr,
                                  nullptr, nullptr, &Controls, nullptr, nullptr,
                                  &Runtime)
              .run();
      if (Mutation != 0 && Mutation != 5 && Mutation != 6) {
        EXPECT_FALSE(Result.Complete);
        EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
        continue;
      }
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      ASSERT_TRUE(Result.Complete);
      ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
      ASSERT_EQ(Result.SEHJumpCaptures.size(), 2u);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 2u);
      const auto Pending =
          llvm::find_if(Result.SEHJumpResumptions, [](const auto &Resume) {
            return Resume.Address == 0x120b;
          });
      ASSERT_NE(Pending, Result.SEHJumpResumptions.end());
      EXPECT_EQ(Pending->CaptureCall, 0x1180u);
      EXPECT_TRUE(Pending->ResumedLevels.empty());
      EXPECT_TRUE(Pending->StateRestores.empty());
      EXPECT_TRUE(Pending->SelectedScopes.empty());
      EXPECT_EQ(Pending->Replacements,
                (std::vector<RegistrationSEHJumpReplacement>{
                    {0x180b, 5, 0x1800, 0x1100}}));
      const auto Actual =
          llvm::find_if(Result.SEHJumpResumptions, [](const auto &Resume) {
            return Resume.Address == 0x180b;
          });
      ASSERT_NE(Actual, Result.SEHJumpResumptions.end());
      EXPECT_EQ(Actual->CaptureCall, 0x1100u);
      EXPECT_EQ(Actual->ResumedLevels,
                (std::vector<int32_t>{Protocol ? -2 : -1}));
      EXPECT_TRUE(Actual->Replacements.empty());
    }
}

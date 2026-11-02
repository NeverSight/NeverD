//===- RegistrationBoundCallStateTests.cpp - Caller-owned SEH borrows -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

TEST(RegistrationBoundCallState,
     ConditionalResultsReachTheNextInitializedCall) {
  for (bool Temporary : {false, true})
    for (uint16_t Width : {2, 4, 8})
      for (bool OtherVolatile : {false, true})
        for (unsigned ResultKind = 0; ResultKind != 5; ++ResultKind) {
          SCOPED_TRACE(Temporary);
          SCOPED_TRACE(Width);
          SCOPED_TRACE(OtherVolatile);
          SCOPED_TRACE(ResultKind);
          auto F = makeCookieFrame();
          F.ExceptionMetadata->Registration->TryLevelStores = {
              {0x1030, 0x1037, -2}};
          auto Unlink = std::move(F.Blocks[3].Ops);
          F.Blocks[3].Ops.clear();
          addSlotStore(F.Blocks[3], -2);
          for (auto Op : Unlink) {
            Op.Seq = F.Blocks[3].Ops.size();
            F.Blocks[3].Ops.push_back(Op);
          }
          F.Blocks[0].Succs = {1};
          for (unsigned I : {1, 2}) {
            auto &B = F.Blocks[I];
            B.Ops.clear();
            B.Succs = {int(I + 1)};
            B.EndAddr = B.StartAddr + 8;
            if (I == 1) {
              emitOp(B, B.StartAddr, NdOp::INT_ADD, NdVar::tmp(40, 4),
                     {NdVar::reg(x86reg::RBP, 4), NdVar::cst(8, 4)});
              emitOp(B, B.StartAddr, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
                     {NdVar::tmp(40, 4)});
            }
            emitOp(
                B, B.StartAddr, NdOp::STORE, {},
                {NdVar::reg(x86reg::RSP, 4),
                 NdVar::reg(I == 2 && OtherVolatile ? x86reg::RCX : x86reg::RAX,
                            4)});
            const size_t First = B.Ops.size();
            const auto Result =
                I == 1 && Temporary
                    ? NdVar::tmp(64, Width)
                    : NdVar::reg(x86reg::RAX, I == 1 ? Width : 4);
            emitOp(B, B.StartAddr + 3, NdOp::CALL, Result,
                   {NdVar::cst(0x5000 + I * 16, 4)});
            if (I == 1 && Temporary && Width == 4)
              emitOp(B, B.StartAddr + 3, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                     {Result});
            B.InstructionBoundaries = {
                {B.StartAddr, 3, 0, uint32_t(First)},
                {B.StartAddr + 3, 5, First, uint32_t(B.Ops.size() - First)}};
            auto &Call = B.InstructionBoundaries.back();
            Call.Control = LowInstructionControl::Call;
            Call.ControlFlags = LowInstructionControlFlag::Call;
            Call.Immediate = 0x5000 + I * 16;
          }
          ConditionalFrame Inputs;
          Inputs.Arguments.emplace(8, FrameValue{});
          const auto Value = ResultKind == 0 ? FrameValue::constant(37)
                             : ResultKind == 1
                                 ? FrameValue::imageAddress(0x6000)
                                 : FrameValue{};
          unsigned Calls = 0;
          Inputs.ProveCall = [&](const LowOp &Op, const FrameState &Frame,
                                 const std::set<int32_t> &Initialized,
                                 std::optional<FrameValue> &Returned) {
            ++Calls;
            const auto SP =
                Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
            EXPECT_TRUE(SP);
            if (!SP)
              return false;
            for (int32_t Byte = *SP; Byte != *SP + 4; ++Byte)
              EXPECT_TRUE(Initialized.count(Byte));
            const auto Argument = Frame.load(*SP, 4);
            if (Op.Inputs[0].Offset == 0x5010) {
              EXPECT_EQ(Argument, FrameValue{});
              if (ResultKind != 3)
                Returned = Value;
              return ResultKind != 4;
            }
            if (Width == 4 && !OtherVolatile && ResultKind < 3)
              EXPECT_EQ(Argument, Value);
            else
              EXPECT_TRUE(Argument.MayBeFrame);
            return true;
          };
          const std::vector<RegistrationCalleeStackContract> Stacks{
              {0x5010, 0, false}, {0x5020, 0, false}};
          RegistrationStateSolver Solver(F, 0x4000, 0, nullptr, nullptr,
                                         &Stacks, nullptr, nullptr, &Inputs);
          const auto States = Solver.run();
          EXPECT_EQ(Calls, 2u);
          EXPECT_EQ(States.Complete && Inputs.Complete, ResultKind != 4)
              << (States.Diagnostics.empty() ? "" : States.Diagnostics.front());
        }
}

TEST(RegistrationBoundCallState, BorrowedObjectsCannotExposeAdministration) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(Mutation);
      const bool EH4 = Protocol != 0;
      auto F = makeJumpFrame();
      if (EH4)
        initializeEH4JumpFrame(F, Protocol == 2);
      F.Blocks.resize(8);
      F.Blocks[3].Succs = {7, 5};
      auto &Call = F.Blocks[7];
      Call.Id = 7;
      Call.StartAddr = 0x1180;
      Call.EndAddr = 0x1185;
      Call.Succs = {4};
      emitOp(Call, 0x1180, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
             {NdVar::cst(0x3200, 4)});
      Call.InstructionBoundaries = {{0x1180, 5, 0, 1}};
      Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
      Call.InstructionBoundaries[0].ControlFlags =
          LowInstructionControlFlag::Call;
      Call.InstructionBoundaries[0].Immediate = 0x3200;
      SEHJumpRuntime Runtime;
      Runtime.Capture = [&](const LowOp &Op, const FrameState &Frame, bool,
                            size_t &) -> std::optional<SEHJumpCapture> {
        if (Op.Addr != 0x1100)
          return std::nullopt;
        SEHJumpCapture Capture;
        Capture.Call = Op.Addr;
        Capture.ReturnPC = 0x1105;
        Capture.BufferOffset = -144;
        Capture.StackOffset = -192;
        Capture.UnwindTarget = 0x3300;
        if (EH4) {
          Capture.Level = -2;
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
        return Op.Addr == 0x111a;
      };
      BoundCalleeProver Prove = [&](const LowOp &Op, const FrameState &Frame,
                                    size_t &,
                                    const std::map<int32_t, SEHJumpCapture> &)
          -> std::optional<BoundCalleeEffect> {
        if (Op.Addr != 0x1180)
          return std::nullopt;
        BoundCalleeEffect Effect;
        Effect.Reads = Effect.Writes = {{-40, -36}};
        auto &Native = Effect.NativeCall.emplace();
        Native.Address = Op.Addr;
        Native.OpSeq = Op.Seq;
        Native.TargetVA = 0x3200;
        Native.StackOffset =
            *Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
        Native.Arguments = {{RegistrationSEHBoundArgument::Kind::Object, -40}};
        switch (Mutation) {
        case 1:
          Effect.Reads = {{-28, -24}};
          break;
        case 2:
          Effect.Writes = {{-28, -24}};
          break;
        case 3:
          Effect.Reads = {{-29, -27}};
          break;
        case 4:
          Effect.Writes = {{-29, -27}};
          break;
        case 5:
          Effect.Reads = {{-20, -16}};
          break;
        case 6:
          Effect.Writes = {{-4096, -4092}};
          break;
        case 7:
          Effect.Writes = {{-40, -40}};
          break;
        case 8:
          Native.Arguments[0].Offset = -28;
          break;
        case 9:
          Effect.Reads = {{-32, -28}};
          break;
        case 10:
          Effect.Writes = {{0, 4}};
          break;
        }
        return Effect;
      };
      RegistrationStateSolver Solver(F, EH4 ? 0x4000 : 0, 0, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr, &Prove,
                                     &Runtime);
      const auto States = Solver.run();
      const bool Valid = Mutation == 0 ||
                         (!EH4 && (Mutation < 5 || Mutation == 8)) ||
                         (Mutation == 9 && Protocol != 2);
      EXPECT_EQ(States.Complete, Valid);
      EXPECT_EQ(States.SEHJumpResumptionsComplete, Valid);
      EXPECT_EQ(States.HasBoundSEHCallees, Valid);
      if (Valid) {
        EXPECT_EQ(States.SecurityCookiesComplete, EH4);
        ASSERT_EQ(States.SEHBoundCalls.size(), 1u);
        EXPECT_EQ(States.SEHBoundCalls.front().Address, 0x1180u);
        ASSERT_EQ(States.SEHJumpResumptions.size(), 1u);
      }
    }
}

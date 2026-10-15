//===- RegistrationPrivateCallStateTests.cpp - Live capture transfers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace {
void initializeEH4JumpFrame(LowFunc &F) {
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.ScopeTableVA = 0x3000;
  Chain.EHCookieOffset = -28;
  Chain.GSCookieOffset = -2;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.front().Level = -2;
  F.Blocks[5].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-2), 4);
  // Allocate the cookie before installation, keeping the final capture SP.
  F.Blocks[1].Ops.front().Inputs[1] = NdVar::cst(144, 4);
  auto &Entry = F.Blocks.front();
  auto Install = Entry.Ops.back();
  Entry.Ops.pop_back();
  emitOp(Entry, 0x1000, NdOp::COPY, NdVar::tmp(20, 4),
         {NdVar::reg(x86reg::RSP, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Entry, 0x1000, NdOp::LOAD, NdVar::tmp(21, 4), {NdVar::cst(0x4000, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(22, 4),
         {NdVar::tmp(21, 4), NdVar::cst(0x3000, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(23, 4),
         {NdVar::tmp(21, 4), NdVar::reg(x86reg::RBP, 4)});
  for (const auto &[Offset, Value] :
       {std::pair{-8, NdVar::tmp(22, 4)}, {-28, NdVar::tmp(23, 4)}}) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(24, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
    emitOp(Entry, 0x1000, NdOp::STORE, {}, {NdVar::tmp(24, 4), Value});
  }
  Install.Seq = Entry.Ops.size();
  Install.Inputs[1] = NdVar::tmp(20, 4);
  Entry.Ops.push_back(Install);
}
} // namespace

TEST(RegistrationPrivateCallState,
     RetainsOnlyAllocatedBuffersAcrossProvedCalls) {
  for (bool EH4 : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Mutation);
      auto F = makeJumpFrame();
      if (EH4)
        initializeEH4JumpFrame(F);
      F.Blocks.resize(8);
      F.Blocks[3].Succs = {7, 5};
      auto &Call = F.Blocks[7];
      Call.Id = 7;
      Call.StartAddr = 0x1180;
      Call.EndAddr = 0x1185;
      Call.Succs = {4};
      emitOp(Call, 0x1180, Mutation == 2 ? NdOp::INDIR_CALL : NdOp::CALL,
             NdVar::reg(x86reg::RAX, 4), {NdVar::cst(0x3200, 4)});
      Call.InstructionBoundaries = {{0x1180, 5, 0, 1}};
      Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
      Call.InstructionBoundaries[0].ControlFlags =
          LowInstructionControlFlag::Call;
      Call.InstructionBoundaries[0].Immediate = 0x3200;
      if (Mutation == 3 || Mutation == 4 || Mutation == 5) {
        if (Mutation == 3)
          emitOp(Call, 0x1185, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
                 {NdVar::reg(x86reg::RSP, 4), NdVar::cst(48, 4)});
        else if (Mutation == 4)
          emitOp(Call, 0x1185, NdOp::COPY, NdVar::reg(x86reg::RBP, 4),
                 {NdVar::cst(0, 4)});
        else
          // A frame-preserving callee has no independently proved scalar
          // return.
          emitOp(Call, 0x1185, NdOp::STORE, {},
                 {NdVar::cst(0x5000, 4), NdVar::reg(x86reg::RAX, 4)});
        Call.InstructionBoundaries.push_back({0x1185, 3, 1, 1});
        Call.EndAddr += 3;
      }
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
      if (Mutation != 1 && Mutation != 6)
        Runtime.PrivateCall = [](const LowOp &Op, size_t &) {
          return Op.Addr == 0x1180;
        };
      // A stack-only declaration must not substitute for frame/capture privacy.
      const std::vector<RegistrationCalleeStackContract> Stacks = {
          {0x3200, 0, false}};
      RegistrationStateSolver Solver(F, EH4 ? 0x4000 : 0, 0, nullptr, nullptr,
                                     Mutation == 6 ? &Stacks : nullptr, nullptr,
                                     nullptr, nullptr, nullptr, &Runtime);
      const auto States = Solver.run();
      EXPECT_EQ(States.Complete, Mutation == 0);
      EXPECT_EQ(States.SEHJumpResumptionsComplete, Mutation == 0);
      if (Mutation == 0) {
        EXPECT_EQ(States.SecurityCookiesComplete, EH4);
        EXPECT_TRUE(States.RegistrationLifetimeComplete);
        ASSERT_EQ(States.SEHJumpCaptures.size(), 1u);
        ASSERT_EQ(States.SEHJumpResumptions.size(), 1u);
        EXPECT_EQ(States.SEHJumpResumptions[0].CaptureCall, 0x1100u);
        EXPECT_FALSE(States.HasBoundSEHCallees);
        EXPECT_TRUE(States.SEHBoundCalls.empty());
      }
    }
}

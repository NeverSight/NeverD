//===- RegistrationStartupInfoStateTests.cpp - PE32 runtime output bounds -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject partial overlaps between a startup output and frame administration.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace {
LowFunc makeStartupFrame(unsigned Protocol, int32_t Output = -108) {
  auto F = makeJumpFrame(0);
  if (Protocol)
    initializeEH4JumpFrame(F, Protocol == 2);
  auto &Setup = F.Blocks[1];
  emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Output), 4)});
  emitOp(Setup, 0x1086, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
  Setup.InstructionBoundaries.back().OpCount = 3;
  auto &Call = F.Blocks[2];
  Call.Ops[0].Opcode = NdOp::INDIR_CALL;
  Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
  Call.InstructionBoundaries[0].Immediate = 0x4200;
  Call.InstructionBoundaries[0].ControlFlags |=
      LowInstructionControlFlag::Indirect;
  auto &Resume = F.Blocks[3];
  Resume.Ops.erase(Resume.Ops.begin());
  Resume.Ops.pop_back();
  Resume.Ops[0].Inputs[1] = NdVar::cst(12, 4);
  Resume.Succs = {5};
  Resume.EndAddr = 0x110b;
  Resume.InstructionBoundaries = {{0x1108, 3, 0, 1}};
  return F;
}
} // namespace

TEST(RegistrationStartupInfo, OutputOwnsItsWholePossibleWriteExtent) {
  const int32_t Outputs[] = {-108, -90, -24, -193, -68, -188, -189, -109};
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mode = 0; Mode != std::size(Outputs) + 1; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Protocol << ' ' << Mode);
      const int32_t Output = Mode < std::size(Outputs) ? Outputs[Mode] : -108;
      auto F = makeStartupFrame(Protocol, Output);
      SEHJumpRuntime Runtime;
      Runtime.StartupInfo = [Mode, &Outputs](const LowOp &Op, size_t &) {
        return Mode < std::size(Outputs) && Op.Opcode == NdOp::INDIR_CALL &&
               Op.Addr == 0x1100;
      };
      RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                     nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, &Runtime);
      const auto Result = Solver.run();
      const bool Expected = Mode == 0 || Mode == 5 || Mode == 7;
      EXPECT_EQ(Result.HasBoundSEHCallees, Expected);
      if (!Expected)
        continue;
      ASSERT_TRUE(Result.Complete);
      ASSERT_EQ(Result.SEHBoundCalls.size(), 1u);
      const auto &Bound = Result.SEHBoundCalls.front();
      EXPECT_EQ(Bound.CalleeKind,
                RegistrationSEHBoundCall::Kind::GetStartupInfoW);
      EXPECT_EQ(Bound.StackOffset, -192);
      ASSERT_EQ(Bound.Arguments.size(), 1u);
      EXPECT_EQ(Bound.Arguments[0].ArgumentKind,
                RegistrationSEHBoundArgument::Kind::Object);
      EXPECT_EQ(Bound.Arguments[0].Offset, Output);
      EXPECT_EQ(Bound.borrowedObjectBytes(), 68u);
    }
}

TEST(RegistrationStartupInfo,
     PossibleWritesPreserveUnwrittenPointerProvenance) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (int32_t Offset : {-108, -52, -48, -44})
      for (unsigned Width : {2u, 4u})
        for (bool Pointer : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Protocol << ' ' << Offset << ' '
                                            << Width << ' ' << Pointer);
          auto F = makeStartupFrame(Protocol);
          auto &Setup = F.Blocks[1];
          emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(8, 4),
                 {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
          emitOp(Setup, 0x1086, NdOp::STORE, {},
                 {NdVar::tmp(8, 4), Pointer ? NdVar::reg(x86reg::RBP, Width)
                                            : NdVar::cst(0, Width)});
          Setup.InstructionBoundaries.back().OpCount += 2;
          auto &Resume = F.Blocks[3];
          emitOp(Resume, 0x1108, NdOp::INT_ADD, NdVar::tmp(8, 4),
                 {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
          emitOp(Resume, 0x1108, NdOp::LOAD, NdVar::tmp(16, Width),
                 {NdVar::tmp(8, 4)});
          emitOp(Resume, 0x1108, NdOp::STORE, {},
                 {NdVar::cst(0x800000, 4), NdVar::tmp(16, Width)});
          Resume.InstructionBoundaries.back().OpCount += 3;
          SEHJumpRuntime Runtime;
          Runtime.StartupInfo = [](const LowOp &Op, size_t &) {
            return Op.Opcode == NdOp::INDIR_CALL && Op.Addr == 0x1100;
          };
          RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                         nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, &Runtime);
          const auto Result = Solver.run();
          EXPECT_EQ(Result.Complete, !Pointer);
        }
}

TEST(RegistrationStartupInfo, EarlierImportWritesInvalidateTheOutputBorrow) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Protocol << ' ' << Mutation);
      auto F = makeStartupFrame(Protocol);
      auto &Setup = F.Blocks[1];
      const uint32_t Address = Mutation == 0   ? 0x4300
                               : Mutation == 1 ? 0x4200
                               : Mutation == 2 ? 0x4203
                                               : 0x41ff;
      emitOp(Setup, 0x1086, NdOp::STORE, {},
             {NdVar::cst(Address, 4), NdVar::cst(0, Mutation == 2 ? 1 : 4)});
      ++Setup.InstructionBoundaries.back().OpCount;
      SEHJumpRuntime Runtime;
      Runtime.ImportSlots.insert(0x4200);
      Runtime.StartupInfo = [](const LowOp &Op, size_t &) {
        return Op.Opcode == NdOp::INDIR_CALL && Op.Addr == 0x1100;
      };
      RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                     nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, &Runtime);
      const auto Result = Solver.run();
      EXPECT_EQ(Result.HasBoundSEHCallees, Mutation == 0);
      if (!Mutation)
        EXPECT_TRUE(Result.Complete);
    }
}

TEST(RegistrationStartupInfo, CheckedOutputPreservesOnlyLiveImportBindings) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Protocol << ' ' << Mutation);
      auto F = makeStartupFrame(Protocol, Mutation == 4 ? -24 : -108);
      if (Mutation >= 1 && Mutation <= 3) {
        auto &Setup = F.Blocks[1];
        emitOp(Setup, 0x1086, NdOp::STORE, {},
               {NdVar::cst(Mutation == 1   ? 0x4400
                           : Mutation == 2 ? 0x4300
                                           : 0x4303,
                           4),
                NdVar::cst(0, Mutation == 3 ? 1 : 4)});
        ++Setup.InstructionBoundaries.back().OpCount;
      }
      F.Blocks[3].Succs = {10};
      LowBlock Next;
      Next.Id = 10;
      Next.StartAddr = 0x1180;
      Next.EndAddr = 0x118f;
      Next.Succs = {5};
      emitOp(Next, 0x1180, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Next, 0x1180, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(0, 4)});
      emitOp(Next, 0x1182, NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, 4),
             {NdVar::cst(0x4300, 4)});
      // This later stack store needs the second import's returning ABI.
      // The output borrow may preserve a live binding, never restore one.
      emitOp(Next, 0x1188, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(37, 4)});
      Next.InstructionBoundaries = {
          {0x1180, 2, 0, 2}, {0x1182, 6, 2, 1}, {0x1188, 7, 3, 1}};
      auto &Call = Next.InstructionBoundaries[1];
      Call.Control = LowInstructionControl::Call;
      Call.ControlFlags =
          LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
      Call.Immediate = 0x4300;
      F.Blocks.push_back(std::move(Next));
      const std::vector<RegistrationCalleeStackContract> Stacks = {
          {0x4200, 4, true}, {0x4300, 4, true}};
      SEHJumpRuntime Runtime;
      Runtime.ImportSlots.insert(0x4200);
      Runtime.StartupInfo = [Mutation](const LowOp &Op, size_t &) {
        return Mutation != 5 && Op.Opcode == NdOp::INDIR_CALL &&
               Op.Addr == 0x1100;
      };
      RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                     nullptr, &Stacks, nullptr, nullptr,
                                     nullptr, nullptr, &Runtime);
      const auto Result = Solver.run();
      EXPECT_EQ(Result.Complete, Mutation <= 1);
      EXPECT_EQ(Result.SEHBoundCalls.size(), Mutation <= 1 ? 1u : 0u);
    }
}

TEST(RegistrationStartupInfo, CallsNeedIndependentRuntimeBindingEffects) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mode = 0; Mode != 6; ++Mode) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(Mode);
      auto F = makeStartupFrame(Protocol);
      std::vector<RegistrationCalleeStackContract> Stacks;
      if (Mode) {
        const bool Imported = Mode >= 2;
        const bool Indirect = Mode == 2 || Mode == 3;
        const va_t Target = Mode >= 4 ? 0x4400 : 0x4300;
        LowBlock Call;
        Call.Id = 10;
        Call.StartAddr = Imported ? 0x106d : 0x1070;
        Call.EndAddr = Indirect ? 0x1076 : 0x1075;
        Call.Succs = F.Blocks[0].Succs;
        if (Imported) {
          emitOp(Call, 0x106d, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
                 {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
          Call.InstructionBoundaries = {{0x106d, 3, 0, 1}};
        }
        emitOp(Call, 0x1070, Indirect ? NdOp::INDIR_CALL : NdOp::CALL, {},
               {NdVar::cst(Target, 4)});
        Call.InstructionBoundaries.push_back(
            {0x1070, uint8_t(Indirect ? 6 : 5), uint32_t(Imported), 1});
        auto &Insn = Call.InstructionBoundaries.back();
        Insn.Control = LowInstructionControl::Call;
        Insn.ControlFlags = LowInstructionControlFlag::Call;
        if (Indirect)
          Insn.ControlFlags |= LowInstructionControlFlag::Indirect;
        Insn.Immediate = Target;
        F.Blocks[0].Succs = {10};
        F.Blocks.push_back(std::move(Call));
        RegistrationCalleeStackContract Stack{Target, Imported ? 4u : 0u,
                                              Indirect};
        Stack.ImportVeneer = Mode >= 4;
        if (Stack.ImportVeneer)
          Stack.RequiredImports = {0x4300};
        Stack.PreservesImportBindings = Mode == 3 || Mode == 5;
        Stacks.push_back(std::move(Stack));
      }
      SEHJumpRuntime Runtime;
      Runtime.ImportSlots.insert(0x4200);
      Runtime.StartupInfo = [](const LowOp &Op, size_t &) {
        return Op.Opcode == NdOp::INDIR_CALL && Op.Addr == 0x1100;
      };
      RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                     nullptr, &Stacks, nullptr, nullptr,
                                     nullptr, nullptr, &Runtime);
      const auto Result = Solver.run();
      const bool Preserved = Mode == 0 || Mode == 3 || Mode == 5;
      EXPECT_EQ(Result.HasBoundSEHCallees, Preserved);
      if (Preserved)
        EXPECT_TRUE(Result.Complete);
    }
}

TEST(RegistrationStartupInfo, LaterIterationsRevokeEarlierRuntimeReceipts) {
  for (bool Loop : {false, true}) {
    SCOPED_TRACE(Loop);
    auto F = makeStartupFrame(0);
    // An explicit physical stack restoration remains valid even when the
    // preceding call no longer has an authenticated imported ABI.
    auto &Resume = F.Blocks[3];
    Resume.Ops[0].Inputs[0] = NdVar::reg(x86reg::RBP, 4);
    Resume.Ops[0].Inputs[1] = NdVar::cst(uint32_t(-176), 4);
    if (Loop) {
      Resume.Succs = {5, 10};
      LowBlock Call;
      Call.Id = 10;
      Call.StartAddr = 0x1180;
      Call.EndAddr = 0x1185;
      Call.Succs = {11};
      emitOp(Call, 0x1180, NdOp::CALL, {}, {NdVar::cst(0x4300, 4)});
      Call.InstructionBoundaries = {{0x1180, 5, 0, 1}};
      Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
      Call.InstructionBoundaries[0].Immediate = 0x4300;
      F.Blocks.push_back(std::move(Call));
      LowBlock Reset;
      Reset.Id = 11;
      Reset.StartAddr = 0x1190;
      Reset.EndAddr = 0x1199;
      Reset.Succs = {2};
      emitOp(Reset, 0x1190, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-192), 4)});
      emitOp(Reset, 0x1193, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-108), 4)});
      emitOp(Reset, 0x1196, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
      Reset.InstructionBoundaries = {
          {0x1190, 3, 0, 1}, {0x1193, 3, 1, 1}, {0x1196, 3, 2, 1}};
      F.Blocks.push_back(std::move(Reset));
    }
    const std::vector<RegistrationCalleeStackContract> Stacks = {
        {0x4300, 0, false}};
    SEHJumpRuntime Runtime;
    Runtime.ImportSlots.insert(0x4200);
    Runtime.StartupInfo = [](const LowOp &Op, size_t &) {
      return Op.Opcode == NdOp::INDIR_CALL && Op.Addr == 0x1100;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, &Stacks, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.SEHBoundCallsComplete, !Loop);
    EXPECT_EQ(Result.SEHBoundCalls.size(), Loop ? 0u : 1u);
  }
}

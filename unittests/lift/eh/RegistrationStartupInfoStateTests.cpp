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

#include "neverd/lift/X86Regs.h"

#include <cstdint>
#include <iterator>

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

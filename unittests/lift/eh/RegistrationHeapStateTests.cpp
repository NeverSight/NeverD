//===- RegistrationHeapStateTests.cpp - Exact process heap policy ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the no-borrow heap request separate from arbitrary heap operations.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

LowFunc heapPolicyFrame(unsigned Protocol, unsigned Mutation) {
  auto F = makeJumpFrame();
  if (Protocol)
    initializeEH4JumpFrame(F, Protocol == 2);
  auto &Setup = F.Blocks[1];
  const auto &Arguments = RegistrationSEHBoundCall::HeapTerminationArguments;
  for (size_t I = 0; I != Arguments.size(); ++I) {
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(8, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
    auto Value = NdVar::cst(Arguments[I] + (Mutation == I + 1),
                            Mutation == 6 && I == 1 ? 2 : 4);
    if (Mutation == 5 && I == 0)
      Value = NdVar::reg(x86reg::RAX, 4);
    emitOp(Setup, 0x1086, NdOp::STORE, {}, {NdVar::tmp(8, 4), Value});
    Setup.InstructionBoundaries.back().OpCount += 2;
  }
  if (Mutation == 7 || Mutation == 8 || Mutation == 10 || Mutation == 11) {
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::cst(Mutation == 7    ? 0x4200
                       : Mutation == 8  ? 0x4203
                       : Mutation == 10 ? 0x4300
                                        : 0x4400,
                       4),
            NdVar::cst(0, Mutation == 8 ? 1 : 4)});
    ++Setup.InstructionBoundaries.back().OpCount;
  }
  auto &Call = F.Blocks[2];
  Call.EndAddr = 0x1106;
  Call.Ops[0].Opcode = NdOp::INDIR_CALL;
  Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
  Call.InstructionBoundaries[0].Size = 6;
  Call.InstructionBoundaries[0].Immediate = 0x4200;
  Call.InstructionBoundaries[0].ControlFlags |=
      LowInstructionControlFlag::Indirect;
  auto &Resume = F.Blocks[3];
  Resume.StartAddr = 0x1108;
  Resume.EndAddr = 0x110f;
  Resume.Ops.clear();
  Resume.Succs = {5};
  emitOp(Resume, 0x1108, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(37, 4)});
  Resume.InstructionBoundaries = {{0x1108, 7, 0, 1}};
  return F;
}

TEST(RegistrationHeapPolicy, OnlyTheExactLiveNoBorrowInvocationIsRecorded) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Protocol << ' ' << Mutation);
      auto F = heapPolicyFrame(Protocol, Mutation);
      SEHJumpRuntime Runtime;
      Runtime.ImportSlots.insert(0x4200);
      Runtime.HeapTermination = [Mutation](const LowOp &Op, size_t &) {
        return Mutation != 9 && Op.Opcode == NdOp::INDIR_CALL &&
               Op.Addr == 0x1100;
      };
      const std::vector<RegistrationCalleeStackContract> Stacks = {
          {0x4200, 16, true}, {0x4300, 4, true}};
      RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                     nullptr, &Stacks, nullptr, nullptr,
                                     nullptr, nullptr, &Runtime);
      const auto Result = Solver.run();
      const bool Expected = Mutation == 0 || Mutation == 11;
      ASSERT_EQ(Result.SEHBoundCalls.size(), Expected ? 1u : 0u);
      if (!Expected)
        continue;
      EXPECT_TRUE(Result.Complete);
      const auto &Call = Result.SEHBoundCalls.front();
      EXPECT_EQ(Call.CalleeKind,
                RegistrationSEHBoundCall::Kind::HeapTermination);
      EXPECT_FALSE(Call.ConstantReturn);
      EXPECT_EQ(Call.StackOffset, -192);
      ASSERT_EQ(Call.Arguments.size(), 4u);
      for (size_t I = 0; I != Call.Arguments.size(); ++I) {
        const auto &Arg = Call.Arguments[I];
        EXPECT_EQ(Arg.ArgumentKind,
                  I == 0 || I == 2
                      ? RegistrationSEHBoundArgument::Kind::NullPointer
                      : RegistrationSEHBoundArgument::Kind::ConstantScalar);
        EXPECT_EQ(Arg.ScalarValue,
                  RegistrationSEHBoundCall::HeapTerminationArguments[I]);
      }
      RegistrationStateSolver Exhausted(F, Protocol ? 0x4000 : 0, 0, nullptr,
                                        nullptr, &Stacks, nullptr, nullptr,
                                        nullptr, nullptr, &Runtime);
      EXPECT_TRUE(Exhausted.run(limits::kMaxRegistrationEHStateWork)
                      .SEHBoundCalls.empty());
    }
}
} // namespace

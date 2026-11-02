//===- RegistrationCallbackLoopTests.cpp - Bounded SEH callbacks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguishes finite callback iterations from recurrent alternatives.

#include "RegistrationFilterTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <utility>

using namespace neverd;
using namespace neverd::registration_filter_test;
using namespace neverd::registration_test;

namespace {
LowFunc loopFinally(unsigned Protocol, bool Memory, unsigned Mutation) {
  auto F = filterFrame(Variant::Correlated, Protocol);
  auto &Entry = F.Blocks[9];
  const va_t Initialize = Entry.Ops.back().Addr;
  Entry.Ops.pop_back();
  Entry.InstructionBoundaries.pop_back();
  const size_t InitFirst = Entry.Ops.size();
  const auto Initial = Mutation == 1
                           ? NdVar::reg(x86reg::RDX, 4)
                           : NdVar::cst(Mutation == 8 ? INT32_MAX : 3, 4);
  if (Memory) {
    emitOp(Entry, Initialize, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-72), 4)});
    emitOp(Entry, Initialize, NdOp::STORE, {}, {NdVar::tmp(0, 4), Initial});
  } else {
    emitOp(Entry, Initialize, NdOp::COPY, NdVar::reg(x86reg::RCX, 4),
           {Initial});
  }
  const uint16_t InitSize = Memory ? 7 : 5;
  Entry.InstructionBoundaries.push_back(
      {Initialize, InitSize, InitFirst, Entry.Ops.size() - InitFirst});
  Entry.EndAddr = Initialize + InitSize;
  Entry.Succs = {int(F.Blocks.size())};
  LowBlock Loop;
  Loop.Id = F.Blocks.size();
  Loop.StartAddr = Entry.EndAddr;
  const auto Counter = Memory ? NdVar::tmp(4, 4) : NdVar::reg(x86reg::RCX, 4);
  if (Memory) {
    emitOp(Loop, Loop.StartAddr, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-72), 4)});
    emitOp(Loop, Loop.StartAddr, NdOp::LOAD, Counter, {NdVar::tmp(0, 4)});
  }
  if (Mutation == 2)
    emitOp(Loop, Loop.StartAddr, NdOp::COPY, Counter, {NdVar::cst(3, 4)});
  else
    emitOp(Loop, Loop.StartAddr, NdOp::INT_SUB, Counter,
           {Counter, NdVar::cst(1, 4)});
  if (Memory)
    emitOp(Loop, Loop.StartAddr, NdOp::STORE, {}, {NdVar::tmp(0, 4), Counter});
  emitOp(Loop, Loop.StartAddr, NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
         {Counter, NdVar::cst(0, 4)});
  const uint16_t StepSize = Memory ? 3 : 1;
  Loop.InstructionBoundaries.push_back(
      {Loop.StartAddr, StepSize, 0, Loop.Ops.size()});
  va_t Branch = Loop.StartAddr + StepSize;
  if (Mutation == 6) {
    const size_t First = Loop.Ops.size();
    emitOp(Loop, Branch, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-143), 4)});
    emitOp(Loop, Branch, NdOp::STORE, {}, {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
    Loop.InstructionBoundaries.push_back({Branch, 7, First, 2});
    Branch += 7;
  }
  const size_t BranchFirst = Loop.Ops.size();
  emitOp(Loop, Branch, NdOp::BOOL_NOT, NdVar::tmp(0, 1),
         {NdVar::reg(x86reg::ZF, 1)});
  emitOp(Loop, Branch, NdOp::COND_BR, {},
         {NdVar::cst(Loop.StartAddr + (Mutation == 4 ? 1 : 0), 8),
          Mutation == 3 ? NdVar::reg(x86reg::RBP, 4) : NdVar::tmp(0, 1)});
  Loop.InstructionBoundaries.push_back({Branch, 2, BranchFirst, 2});
  Loop.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Loop.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  Loop.EndAddr = Branch + 2;
  Loop.Succs = {Loop.Id, Loop.Id + 1};
  if (Mutation == 9)
    std::reverse(Loop.Succs.begin(), Loop.Succs.end());
  LowBlock Exit;
  Exit.Id = Loop.Id + 1;
  Exit.StartAddr = Loop.EndAddr;
  va_t Return = Exit.StartAddr;
  if (Mutation == 7) {
    emitOp(Exit, Return, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    Exit.InstructionBoundaries.push_back({Return, 3, 0, 1});
    Return += 3;
  }
  const size_t ReturnFirst = Exit.Ops.size();
  emitOp(Exit, Return, NdOp::RETURN, {}, {});
  Exit.InstructionBoundaries.push_back({Return, 1, ReturnFirst, 1});
  Exit.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  Exit.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  Exit.EndAddr = Return + 1;
  F.Blocks.push_back(std::move(Loop));
  F.Blocks.push_back(std::move(Exit));
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  if (Mutation == 5)
    std::reverse(F.Blocks.begin(), F.Blocks.end());
  return F;
}
} // namespace

TEST(RegistrationCallbackLoop, CompleteFramesProveFiniteIterations) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (bool Memory : {false, true})
      for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
        SCOPED_TRACE(Protocol);
        SCOPED_TRACE(Memory);
        SCOPED_TRACE(Mutation);
        const auto Result = analyze(loopFinally(Protocol, Memory, Mutation),
                                    Variant::Correlated);
        if (Mutation && Mutation != 5 && Mutation != 9) {
          EXPECT_FALSE(Result.Complete);
          EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
          continue;
        }
        for (const auto &Diagnostic : Result.Diagnostics)
          ADD_FAILURE() << Diagnostic;
        EXPECT_TRUE(Result.Complete);
        EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
        ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
        EXPECT_EQ(Result.SEHJumpResumptions.front().ResumedLevels,
                  (std::vector<int32_t>{Protocol ? -2 : -1, 0}));
      }
}

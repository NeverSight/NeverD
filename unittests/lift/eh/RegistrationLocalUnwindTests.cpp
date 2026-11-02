//===- RegistrationLocalUnwindTests.cpp - SEH4 local unwind arguments ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cookie identity and actual stack arguments precede local cleanup effects.

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

LowFunc localUnwindFrame(bool GS, unsigned Mutation) {
  auto Function = makeCookieFrame(GS);
  auto &Chain = *Function.ExceptionMetadata->Registration;
  Chain.Scopes = {{-2, 0, 0x1800, true}};
  Function.Blocks.resize(7);
  Function.Blocks[0].Succs = {4};
  Function.Blocks[0].InstructionBoundaries.front().OpCount =
      Function.Blocks[0].Ops.size();
  Function.Blocks[1].Succs = {5};
  auto &Setup = Function.Blocks[4];
  Setup.Id = 4;
  Setup.StartAddr = 0x1080;
  Setup.EndAddr = 0x1083;
  Setup.Succs = {1};
  emitOp(Setup, 0x1080, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1080, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::reg(x86reg::RSP, 4)});
  if (Mutation == 7) {
    emitOp(Setup, 0x1080, NdOp::INT_ADD, NdVar::tmp(1, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Setup, 0x1080, NdOp::STORE, {},
           {NdVar::tmp(1, 4), NdVar::cst(0, 4)});
  }
  if (Mutation >= 14 && Mutation <= 16)
    emitOp(Setup, 0x1080, NdOp::STORE, {},
           {NdVar::cst(Mutation == 14   ? 0x5000
                       : Mutation == 15 ? 0x5003
                                        : 0x5004,
                       4),
            NdVar::cst(0, Mutation == 15 ? 1 : 4)});
  Setup.InstructionBoundaries = {{0x1080, 3, 0, Setup.Ops.size()}};
  auto &Call = Function.Blocks[5];
  Call.Id = 5;
  Call.StartAddr = 0x1100;
  Call.Succs = {2};
  va_t Next = Call.StartAddr;
  auto Push = [&](NdVar Value, unsigned Size) {
    const size_t First = Call.Ops.size();
    emitOp(Call, Next, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, Next, NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), Value});
    Call.InstructionBoundaries.push_back({Next, uint16_t(Size), First, 2});
    Next += Size;
  };
  Push(Mutation == 4 ? NdVar::reg(x86reg::RCX, 4)
                     : NdVar::cst(Mutation == 12 ? 0 : uint32_t(-2), 4),
       2);
  const size_t First = Call.Ops.size();
  emitOp(Call, Next, NdOp::INT_ADD, NdVar::reg(x86reg::RAX, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 3 ? -12 : -16), 4)});
  Call.InstructionBoundaries.push_back({Next, 3, First, 1});
  Next += 3;
  Push(NdVar::reg(x86reg::RAX, 4), 1);
  Push(Mutation == 2 ? NdVar::reg(x86reg::RDX, 4)
                     : NdVar::cst(Mutation == 1 ? 0x4004 : 0x4000, 4),
       5);
  const size_t CallIndex = Call.Ops.size();
  emitOp(Call, Next, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x2100, 4)});
  if (Mutation == 10)
    Call.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  if (Mutation == 11)
    Call.Ops.back().MemoryOrdering = NdMemoryOrdering::Acquire;
  Call.EndAddr = Next + 5;
  Call.InstructionBoundaries.push_back({Next, 5, CallIndex, 1});
  auto &Boundary = Call.InstructionBoundaries.back();
  Boundary.Control = LowInstructionControl::Call;
  Boundary.ControlFlags = LowInstructionControlFlag::Call;
  Boundary.Immediate = 0x2100;
  if (Mutation == 9)
    Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
  if (Mutation == 13)
    Boundary.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
  if (Mutation == 6)
    Function.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH3;
  auto &Finally = Function.Blocks[6];
  Finally.Id = 6;
  Finally.StartAddr = 0x1800;
  Finally.EndAddr = 0x1804;
  emitOp(Finally, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-40), 4)});
  emitOp(Finally, 0x1800, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::reg(x86reg::RBP, 4)});
  emitOp(Finally, 0x1803, NdOp::RETURN, {}, {});
  Finally.InstructionBoundaries = {{0x1800, 3, 0, 2}, {0x1803, 1, 2, 1}};
  Finally.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  Finally.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  auto &Resume = Function.Blocks[2];
  emitOp(Resume, 0x1020, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-40), 4)});
  emitOp(Resume, 0x1020, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(0, 4)});
  Resume.InstructionBoundaries.front().OpCount = Resume.Ops.size();
  return Function;
}

} // namespace

TEST(RegistrationLocalUnwind, EH4RequiresCookieFrameAndBoundBeforeCleanup) {
  for (bool GS : {false, true})
    for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
      SCOPED_TRACE(GS);
      SCOPED_TRACE(Mutation);
      auto Function = localUnwindFrame(GS, Mutation);
      using Kind = RegistrationLocalUnwindContract::Kind;
      const std::vector<RegistrationLocalUnwindContract> Calls{
          {0x2100, false, Mutation == 5 ? Kind::EH3 : Kind::EH4, 0x5000}};
      const auto States =
          analyzeRegistrationStates(Function, Mutation == 8 ? 0 : 0x4000, 0,
                                    nullptr, nullptr, nullptr, &Calls);
      const bool Valid = Mutation == 0 || Mutation == 12 || Mutation == 16;
      EXPECT_EQ(States.Complete, Valid);
      if (States.HasSEHLocalUnwinds)
        EXPECT_EQ(States.SEHLocalUnwindsComplete, Valid);
      if (!Valid) {
        EXPECT_TRUE(States.SEHLocalUnwinds.empty());
        continue;
      }
      ASSERT_TRUE(States.HasSEHLocalUnwinds);
      ASSERT_EQ(States.SEHLocalUnwinds.size(), 1u);
      const auto &Call = States.SEHLocalUnwinds.front();
      EXPECT_EQ(Call.Address, Function.Blocks[5].Ops.back().Addr);
      EXPECT_EQ(Call.EndAddress, Function.Blocks[5].EndAddr);
      EXPECT_EQ(Call.OpSeq, Function.Blocks[5].Ops.back().Seq);
      EXPECT_EQ(Call.Runtime, Calls.front());
      EXPECT_EQ(Call.StackOffset, -52);
      EXPECT_EQ(Call.EntryVA, 0u);
      EXPECT_EQ(Call.Bound, Mutation == 12 ? 0u : uint32_t(-2));
      EXPECT_EQ(Call.Transitions, (std::vector<std::pair<int32_t, int32_t>>{
                                      {0, Mutation == 12 ? 0 : -2}}));
      EXPECT_TRUE(States.SecurityCookiesComplete);
      ASSERT_EQ(States.Blocks.size(), Function.Blocks.size());
      EXPECT_EQ(States.Blocks[2].levels(),
                (std::vector<int32_t>{Mutation == 12 ? 0 : -2}));
      const auto Seq = Function.Blocks[2].Ops.back().Seq;
      const auto Marker = llvm::find_if(States.FrameValues, [&](const auto &V) {
        return V.Address == 0x1020 && V.OpSeq == Seq;
      });
      if (Mutation != 12) {
        ASSERT_NE(Marker, States.FrameValues.end());
        EXPECT_EQ(Marker->EstablishedFrameOffset, 0);
      } else {
        EXPECT_EQ(Marker, States.FrameValues.end());
      }
      // State analysis alone still grants no native call-frame authority.
      EXPECT_FALSE(States.CallFrameEffectsComplete);
    }
}

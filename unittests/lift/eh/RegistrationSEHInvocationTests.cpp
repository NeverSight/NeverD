//===- RegistrationSEHInvocationTests.cpp - SEH parent resumption ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tests ordinary and runtime SEH invocation exits and their negative controls.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::registration_test;

namespace {

LowFunc invocationFrame(unsigned Mutation, bool Returning) {
  auto F = makeBranchingFrame();
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes = {{-1, 0, 0x1850, true}, {0, 0, 0x1800, true}};
  F.Blocks.resize(11);
  auto &UnownedResume = F.Blocks[10];
  UnownedResume.Id = 10;
  UnownedResume.StartAddr = 0x1900;
  UnownedResume.EndAddr = 0x1901;
  emitOp(UnownedResume, 0x1900, NdOp::RETURN, {}, {});
  UnownedResume.InstructionBoundaries = {{0x1900, 1, 0, 1}};
  UnownedResume.InstructionBoundaries.back().Control =
      LowInstructionControl::Return;
  UnownedResume.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  F.Blocks[7].Id = 7;
  F.Blocks[7].StartAddr = 0x1820;
  F.Blocks[7].EndAddr = 0x1821;
  F.Blocks[0].Succs = {4};
  F.Blocks[1].Succs = {5};
  auto &Setup = F.Blocks[4];
  Setup.Id = 4;
  Setup.StartAddr = 0x1080;
  Setup.EndAddr = 0x1088;
  Setup.Succs = {1};
  emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(40, 4)});
  emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1086, NdOp::STORE, {},
         {NdVar::reg(x86reg::RCX, 4),
          Mutation == 3 ? NdVar::cst(0, 4) : NdVar::reg(x86reg::RSP, 4)});
  Setup.InstructionBoundaries = {
      {0x1080, 3, 0, 1}, {0x1083, 3, 1, 1}, {0x1086, 2, 2, 1}};
  auto &Call = F.Blocks[5];
  Call.Id = 5;
  Call.StartAddr = 0x1100;
  Call.EndAddr = 0x1105;
  Call.Succs = {8};
  emitOp(Call, 0x1100, NdOp::CALL, {}, {NdVar::cst(0x1800, 4)});
  Call.InstructionBoundaries = {{0x1100, 5, 0, 1}};
  Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Call.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Call;
  Call.InstructionBoundaries.back().Immediate = 0x1800;
  if (Mutation == 9)
    Call.InstructionBoundaries.back().ControlFlags |=
        LowInstructionControlFlag::Conditional;
  auto &Finally = F.Blocks[6];
  Finally.Id = 6;
  Finally.StartAddr = 0x1800;
  if (Returning) {
    emitOp(Finally, 0x1800, NdOp::RETURN, {}, {});
    Finally.EndAddr = 0x1801;
    Finally.InstructionBoundaries = {{0x1800, 1, 0, 1}};
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    Finally.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    if (Mutation == 10)
      Finally.InstructionBoundaries.back().Immediate = 4;
  } else {
    emitOp(Finally, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4),
            NdVar::cst(uint32_t(Mutation == 2 ? -20 : -24), 4)});
    emitOp(Finally, 0x1800, NdOp::LOAD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::tmp(0, 4)});
    emitOp(Finally, 0x1803, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Finally, 0x1803, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(UINT32_MAX, 4)});
    emitOp(Finally, 0x1805, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Finally, 0x1808, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Finally, 0x1808, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4),
            Mutation == 6 ? NdVar::cst(0, 4) : NdVar::reg(x86reg::RDX, 4)});
    emitOp(Finally, 0x1809, NdOp::CALL, {},
           {NdVar::cst(Mutation == 5 ? 0x2300 : 0x2200, 4)});
    Finally.EndAddr = 0x180e;
    Finally.Succs = {7};
    Finally.InstructionBoundaries = {{0x1800, 3, 0, 2},
                                     {0x1803, 2, 2, 2},
                                     {0x1805, 3, 4, 1},
                                     {0x1808, 1, 5, 2},
                                     {0x1809, 5, 7, 1}};
    Finally.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Finally.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Finally.InstructionBoundaries.back().Immediate =
        Mutation == 5 ? 0x2300 : 0x2200;
    auto &Jump = F.Blocks[7];
    Jump.Id = 7;
    Jump.StartAddr = 0x180e;
    Jump.EndAddr = 0x1813;
    Jump.Succs = Mutation == 8    ? std::vector<int>{}
                 : Mutation == 7  ? std::vector<int>{9}
                 : Mutation == 12 ? std::vector<int>{10}
                                  : std::vector<int>{8};
    emitOp(Jump, 0x180e, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Mutation == 4 ? 4 : 8, 4)});
    emitOp(Jump, 0x1811, NdOp::BRANCH, {},
           {NdVar::cst(Mutation == 7    ? 0x1850
                       : Mutation == 12 ? 0x1900
                                        : 0x1105,
                       Mutation == 11 ? 2 : 8)});
    Jump.InstructionBoundaries = {{0x180e, 3, 0, 1}, {0x1811, 2, 1, 1}};
    Jump.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
    Jump.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Branch;
    Jump.InstructionBoundaries.back().Immediate = Mutation == 7    ? 0x1850
                                                  : Mutation == 12 ? 0x1900
                                                                   : 0x1105;
  }
  auto &Resume = F.Blocks[8];
  Resume.Id = 8;
  Resume.StartAddr = 0x1105;
  Resume.EndAddr = 0x110c;
  Resume.Succs = {2};
  emitOp(Resume, 0x1105, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::reg(x86reg::RSP, 4)});
  emitOp(Resume, 0x1107, NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)});
  Resume.InstructionBoundaries = {{0x1105, 2, 0, 1}, {0x1107, 5, 1, 1}};
  Resume.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Resume.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch;
  Resume.InstructionBoundaries.back().Immediate = 0x1020;
  auto &OuterFinally = F.Blocks[9];
  OuterFinally.Id = 9;
  OuterFinally.StartAddr = 0x1850;
  OuterFinally.EndAddr = 0x1851;
  emitOp(OuterFinally, 0x1850, NdOp::RETURN, {}, {});
  OuterFinally.InstructionBoundaries = {{0x1850, 1, 0, 1}};
  OuterFinally.InstructionBoundaries.back().Control =
      LowInstructionControl::Return;
  OuterFinally.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  return F;
}

} // namespace

TEST(RegistrationState, LocalUnwindKeepsOrdinaryAndRuntimeFinallyEvidence) {
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    auto F = invocationFrame(0, false);
    // The inner finally can run during exception dispatch before its parent
    // retires the scope for the ordinary CALL. Both paths share one receipt.
    F.Blocks.resize(12);
    F.Blocks[4].Succs = {11};
    auto &Active = F.Blocks[11];
    Active.Id = 11;
    Active.StartAddr = 0x1090;
    Active.EndAddr = 0x1097;
    Active.Succs = {1};
    addSlotStore(Active, 1);
    Active.InstructionBoundaries = {{0x1090, 7, 0, Active.Ops.size()}};
    F.ExceptionMetadata->Registration->TryLevelStores.push_back(
        {0x1090, 0x1097, 1});
    auto &Exit = F.Blocks[3];
    emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(30, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::tmp(31, 4),
           {NdVar::tmp(30, 4)});
    emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
           {NdVar::cst(0, 4), NdVar::tmp(31, 4)}, NdMemoryAddressSpace::X86FS);
    Exit.InstructionBoundaries.front().OpCount = Exit.Ops.size();
    if (Reverse)
      std::reverse(F.Blocks.begin(), F.Blocks.end());
    const std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
         0x1800},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
         InvalidVA}};
    const std::vector<RegistrationLocalUnwindContract> Unwinds{{0x2200, false}};
    const auto States = analyzeRegistrationStates(F, 0, 0, nullptr, nullptr,
                                                  nullptr, &Unwinds, &Controls);
    ASSERT_TRUE(States.Complete);
    ASSERT_TRUE(States.CallbackStatesComplete);
    ASSERT_TRUE(States.RegistrationLifetimeComplete);
    ASSERT_TRUE(States.HasSEHLocalUnwinds);
    ASSERT_TRUE(States.SEHLocalUnwindsComplete);
    ASSERT_EQ(States.SEHLocalUnwinds.size(), 1u);
    const auto &Call = States.SEHLocalUnwinds.front();
    EXPECT_EQ(Call.Address, 0x1809u);
    EXPECT_EQ(Call.EntryVA, 0x1800u);
    EXPECT_EQ(Call.StackOffset, -64);
    EXPECT_EQ(Call.Bound, UINT32_MAX);
    EXPECT_FALSE(Call.Transitions.empty());
  }
}

TEST(RegistrationState, FinallyInvocationsResumeOnlyTheirCheckedParent) {
  for (bool Returning : {false, true})
    for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
      SCOPED_TRACE(Returning);
      SCOPED_TRACE(Mutation);
      auto F = invocationFrame(Mutation, Returning);
      std::vector<RegistrationSEHControlContract> Controls{
          {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
           0x1800},
          {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
           InvalidVA}};
      if (Mutation == 1)
        Controls.erase(Controls.begin());
      if (Returning && Mutation != 10)
        Controls.push_back({RegistrationSEHControlContract::Kind::NearReturn,
                            0x1800, 0x1801, InvalidVA});
      const std::vector<RegistrationLocalUnwindContract> Unwinds{
          {0x2200, false}};
      const auto A = analyzeRegistrationStates(F, 0, 0, nullptr, nullptr,
                                               nullptr, &Unwinds, &Controls);
      bool Restored = false;
      for (const auto &Value : A.FrameValues)
        if (Value.Address == 0x1105 && Value.OpSeq == 0)
          Restored |= Value.EstablishedFrameOffset == -56;
      const bool Valid =
          Mutation == 0 ||
          (Returning && Mutation >= 2 && Mutation != 9 && Mutation != 10) ||
          (!Returning && Mutation == 10);
      EXPECT_EQ(A.Complete, Valid);
      EXPECT_EQ(Restored, Valid);
      EXPECT_EQ(A.SEHInvocationExitsComplete, Valid);
      if (!A.SEHInvocationExitsComplete)
        EXPECT_TRUE(A.SEHInvocationExits.empty());
      if (Valid) {
        ASSERT_EQ(A.SEHInvocationExits.size(), 1u);
        const auto &Exit = A.SEHInvocationExits.front();
        EXPECT_EQ(Exit.EntryVA, 0x1800u);
        EXPECT_EQ(Exit.CallVA, 0x1100u);
        EXPECT_EQ(Exit.TargetVA, 0x1105u);
        EXPECT_EQ(Exit.StackOffset, -56);
        EXPECT_EQ(Exit.Exit,
                  Returning ? RegistrationSEHInvocationExit::Kind::NearReturn
                            : RegistrationSEHInvocationExit::Kind::ParentJump);
        if (!Returning) {
          EXPECT_EQ(Exit.StackRestoreVA, 0x1800u);
          EXPECT_EQ(Exit.StackRestoreSeq, 1);
        }
      }
      EXPECT_FALSE(A.CallFrameEffectsComplete);
    }
}

TEST(RegistrationState, FinallyOnlyParentTailsRequireEveryPathToAnAnchor) {
  for (unsigned Shape = 0; Shape != 3; ++Shape)
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation)
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(Shape);
        SCOPED_TRACE(Mutation);
        SCOPED_TRACE(Reverse);
        auto F = invocationFrame(12, false);
        F.Blocks.resize(Shape == 2 ? 13 : Shape == 1 ? 12 : 11);
        const auto SetBranch = [&](unsigned Id, va_t Address, unsigned Target,
                                   va_t TargetAddress) {
          auto &Block = F.Blocks[Id];
          Block = {};
          Block.Id = Id;
          Block.StartAddr = Address;
          Block.EndAddr = Address + 2;
          Block.Succs = {int(Target)};
          emitOp(Block, Address, NdOp::BRANCH, {},
                 {NdVar::cst(TargetAddress, 4)});
          Block.InstructionBoundaries = {{Address, 2, 0, 1}};
          auto &Instruction = Block.InstructionBoundaries.back();
          Instruction.Control = LowInstructionControl::Branch;
          Instruction.ControlFlags = LowInstructionControlFlag::Branch;
          Instruction.Immediate = TargetAddress;
        };
        SetBranch(10, 0x1900, Shape ? 11 : 8, Shape ? 0x1910 : 0x1105);
        if (Shape)
          SetBranch(11, 0x1910, 8, 0x1105);
        if (Shape == 2) {
          SetBranch(12, 0x1902, 8, 0x1105);
          auto &Split = F.Blocks[10];
          Split.Succs.push_back(12);
          Split.Ops.back().Opcode = NdOp::COND_BR;
          Split.Ops.back().addInput(NdVar::cst(1, 1));
          Split.InstructionBoundaries.back().ControlFlags |=
              LowInstructionControlFlag::Conditional;
        }
        auto &Tail = F.Blocks[10];
        switch (Mutation) {
        case 1:
          SetBranch(10, 0x1900, 10, 0x1900);
          break;
        case 2:
          SetBranch(10, 0x1900, 9, 0x1850);
          break;
        case 3:
          Tail.Succs.front() = 99;
          break;
        case 4:
          Tail.Succs.push_back(Tail.Succs.front());
          break;
        case 5:
          ++*Tail.InstructionBoundaries.back().Immediate;
          break;
        case 6:
          F.ExceptionMetadata->CodeRange.End = 0x1900;
          break;
        case 7:
          Tail.InstructionBoundaries.back().TargetMode =
              LowInstructionTargetMode::Thumb;
          break;
        case 8:
          Tail.Succs.clear();
          Tail.Ops.back().Opcode = NdOp::RETURN;
          Tail.Ops.back().NumInputs = 0;
          Tail.InstructionBoundaries.back().Control =
              LowInstructionControl::Return;
          Tail.InstructionBoundaries.back().ControlFlags =
              LowInstructionControlFlag::Return;
          Tail.InstructionBoundaries.back().Immediate = 0;
          break;
        case 9:
          if (Shape == 2)
            SetBranch(12, 0x1902, 9, 0x1850);
          else
            Tail.InstructionBoundaries.back().ControlFlags |=
                LowInstructionControlFlag::InstructionGuard;
          break;
        }
        if (Reverse)
          std::reverse(F.Blocks.begin(), F.Blocks.end());
        const std::vector<RegistrationSEHControlContract> Controls{
            {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
             0x1800},
            {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
             InvalidVA}};
        const std::vector<RegistrationLocalUnwindContract> Unwinds{
            {0x2200, false}};
        const auto State = analyzeRegistrationStates(
            F, 0, 0, nullptr, nullptr, nullptr, &Unwinds, &Controls);
        EXPECT_EQ(State.Complete, Mutation == 0);
        EXPECT_EQ(State.SEHInvocationExitsComplete, Mutation == 0);
        if (Mutation)
          continue;
        ASSERT_EQ(State.SEHInvocationExits.size(), 1u);
        EXPECT_EQ(State.SEHInvocationExits.front().TargetVA, 0x1900u);
        const auto Reached = llvm::find_if(State.Blocks, [](const auto &Block) {
          return Block.BlockId == 10;
        });
        ASSERT_NE(Reached, State.Blocks.end());
        EXPECT_TRUE(Reached->Reached);
        EXPECT_FALSE(Reached->CallbackOnly);
      }
}

TEST(RegistrationState, NestedFinallyKeepsEachInvocationReturnSlot) {
  for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = invocationFrame(0, true);
    auto &Outer = F.Blocks[6];
    Outer.Ops.clear();
    Outer.EndAddr = 0x1805;
    Outer.Succs = {7};
    if (Mutation == 5)
      emitOp(Outer, 0x1800, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(1, 4)});
    emitOp(Outer, 0x1800, NdOp::CALL, {}, {NdVar::cst(0x1850, 4)});
    Outer.InstructionBoundaries = {{0x1800, 5, 0, uint32_t(Outer.Ops.size())}};
    Outer.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Outer.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Outer.InstructionBoundaries.back().Immediate = 0x1850;
    auto &Return = F.Blocks[7];
    Return.StartAddr = 0x1805;
    Return.EndAddr = 0x1806;
    emitOp(Return, 0x1805, NdOp::RETURN, {}, {});
    Return.InstructionBoundaries = {{0x1805, 1, 0, 1}};
    Return.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    Return.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    auto &Inner = F.Blocks[9];
    if (Mutation == 1)
      Inner.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 2 || Mutation == 4) {
      Inner.Ops.clear();
      emitOp(
          Inner, 0x1850, Mutation == 2 ? NdOp::STORE : NdOp::INT_ADD,
          Mutation == 2 ? NdVar{} : NdVar::reg(x86reg::RSP, 4),
          {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Mutation == 2 ? 0 : 4, 4)});
      emitOp(Inner, 0x1851, NdOp::RETURN, {}, {});
      Inner.EndAddr = 0x1852;
      Inner.InstructionBoundaries = {{0x1850, 1, 0, 1}, {0x1851, 1, 1, 1}};
      Inner.InstructionBoundaries.back().Control =
          LowInstructionControl::Return;
      Inner.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Return;
    }
    const bool PrivateStacks = Mutation >= 6;
    if (PrivateStacks) {
      Outer.Ops.clear();
      emitOp(Outer, 0x1800, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Outer, 0x1800, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
      emitOp(Outer, 0x1802, NdOp::CALL, {}, {NdVar::cst(0x1850, 4)});
      Outer.EndAddr = 0x1807;
      Outer.InstructionBoundaries = {{0x1800, 2, 0, 2}, {0x1802, 5, 2, 1}};
      Outer.InstructionBoundaries.back().Control = LowInstructionControl::Call;
      Outer.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Call;
      Outer.InstructionBoundaries.back().Immediate = 0x1850;
      Return.StartAddr = 0x1807;
      Return.EndAddr = 0x1809;
      Return.Ops.clear();
      emitOp(Return, 0x1807, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
             {NdVar::reg(x86reg::RSP, 4)});
      emitOp(Return, 0x1807, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Return, 0x1808, NdOp::RETURN, {}, {});
      Return.InstructionBoundaries = {{0x1807, 1, 0, 2}, {0x1808, 1, 2, 1}};
      Return.InstructionBoundaries.back().Control =
          LowInstructionControl::Return;
      Return.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Return;
      Inner.Ops.clear();
      emitOp(Inner, 0x1850, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Inner, 0x1850, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(9, 4)});
      emitOp(Inner, 0x1852, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
             {NdVar::reg(x86reg::RSP, 4)});
      emitOp(Inner, 0x1852, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Inner, 0x1853, NdOp::RETURN, {}, {});
      Inner.EndAddr = 0x1854;
      Inner.InstructionBoundaries = {
          {0x1850, 2, 0, 2}, {0x1852, 1, 2, 2}, {0x1853, 1, 4, 1}};
      Inner.InstructionBoundaries.back().Control =
          LowInstructionControl::Return;
      Inner.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Return;
    }
    const va_t NestedCall = PrivateStacks ? 0x1802 : 0x1800;
    std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
         0x1800},
        {RegistrationSEHControlContract::Kind::NearCall, NestedCall,
         Outer.EndAddr, 0x1850},
        {RegistrationSEHControlContract::Kind::NearReturn, Return.EndAddr - 1,
         Return.EndAddr, InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, Inner.EndAddr - 1,
         Inner.EndAddr, InvalidVA}};
    if (Mutation == 3)
      Controls.erase(Controls.begin() + 1);
    if (Mutation >= 7 && Mutation < 16)
      addFinallyTailJump(Outer, Return, Controls, Mutation - 7);
    if (Mutation == 16)
      addFinallyCallTrailingEffect(Outer);
    if (Mutation == 17)
      F.ExceptionMetadata->CodeRange.End = 0x1700;
    if (Mutation == 18)
      F.ExceptionMetadata->CodeRange.End = 0x1105;
    const auto A = analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr,
                                             nullptr, &Controls);
    const bool Valid =
        Mutation == 0 || Mutation == 6 || Mutation == 7 || Mutation == 17;
    EXPECT_EQ(A.Complete, Valid);
    if (Valid) {
      EXPECT_FALSE(A.HasBoundSEHCallees);
      ASSERT_EQ(A.SEHInvocationExits.size(), 2u);
      EXPECT_EQ(A.SEHInvocationExits.front().Address, Return.EndAddr - 1);
      EXPECT_EQ(A.SEHInvocationExits.front().CallVA, 0x1100u);
      EXPECT_EQ(A.SEHInvocationExits.front().StackOffset, -56);
      EXPECT_EQ(A.SEHInvocationExits.back().Address, Inner.EndAddr - 1);
      EXPECT_EQ(A.SEHInvocationExits.back().CallVA, NestedCall);
      EXPECT_EQ(A.SEHInvocationExits.back().EntryVA, 0x1850u);
      EXPECT_EQ(A.SEHInvocationExits.back().TargetVA, Return.StartAddr);
      EXPECT_EQ(A.SEHInvocationExits.back().StackOffset,
                PrivateStacks ? -4 : 0);
    } else
      EXPECT_FALSE(A.SEHInvocationExitsComplete);
  }
}

TEST(RegistrationState, FinallyNearReturnsKeepTheirPhysicalFoldedContinuation) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = invocationFrame(0, true);
    auto &Resume = F.Blocks[8];
    Resume.StartAddr += 5;
    Resume.EndAddr += 5;
    for (auto &Op : Resume.Ops)
      Op.Addr += 5;
    for (auto &Boundary : Resume.InstructionBoundaries)
      Boundary.Address += 5;
    std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
         0x1800},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1800, 0x1801,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearJump, 0x1105, 0x1107,
         0x110a}};
    if (Mutation == 1)
      Controls.pop_back();
    if (Mutation == 2)
      Controls.back().Target = 0x1900;
    if (Mutation == 3)
      Controls.back().Target = 0x1105;
    if (Mutation == 4)
      Controls.back().Control =
          RegistrationSEHControlContract::Kind::NearReturn;
    if (Mutation == 5)
      Controls.back().EndAddress = 0x1106;
    if (Mutation == 6)
      Controls.push_back(Controls.back());
    if (Mutation >= 7) {
      auto &Call = F.Blocks[5];
      Call.EndAddr = 0x1107;
      emitOp(Call, 0x1105, NdOp::BRANCH, {}, {NdVar::cst(0x110a, 8)});
      Call.InstructionBoundaries.push_back({0x1105, 2, 1, 1});
      auto &Tail = Call.InstructionBoundaries.back();
      Tail.Control = LowInstructionControl::Branch;
      Tail.ControlFlags = LowInstructionControlFlag::Branch;
      Tail.Immediate = 0x110a;
      if (Mutation == 8)
        Controls.pop_back();
      if (Mutation == 9) {
        Call.Ops.back().Inputs[0].Offset = 0x110b;
        Tail.Immediate = 0x110b;
      }
      if (Mutation == 10)
        Tail.ControlFlags |= LowInstructionControlFlag::Conditional;
      if (Mutation == 11)
        Tail.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
      if (Mutation == 12) {
        // A checked branch cannot authorize an additional register effect
        // after the physical CALL return PC and before callback resumption.
        Call.Ops.back().Opcode = NdOp::COPY;
        Call.Ops.back().Output = NdVar::reg(x86reg::RBP, 4);
        Call.Ops.back().Inputs[0] = NdVar::cst(0, 4);
        emitOp(Call, 0x1105, NdOp::BRANCH, {}, {NdVar::cst(0x110a, 8)});
        Tail.OpCount = 2;
      }
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr,
                                             nullptr, &Controls);
    const bool Valid = Mutation == 0 || Mutation == 7;
    EXPECT_EQ(A.Complete, Valid);
    EXPECT_EQ(A.SEHInvocationExitsComplete, Valid);
    if (Valid) {
      ASSERT_EQ(A.SEHInvocationExits.size(), 1u);
      EXPECT_EQ(A.SEHInvocationExits.front().CallVA, 0x1100u);
      EXPECT_EQ(A.SEHInvocationExits.front().TargetVA, 0x110au);
    } else
      EXPECT_TRUE(A.SEHInvocationExits.empty());
  }
}

TEST(RegistrationState, FinallyWaitKeepsIntegerFramesAndExceptionalDispatch) {
  for (bool InspectReturningBody : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(InspectReturningBody);
      SCOPED_TRACE(Mutation);
      auto F = invocationFrame(0, true);
      auto &Finally = F.Blocks[6];
      Finally.Ops.clear();
      Finally.InstructionBoundaries.clear();
      emitOp(Finally, 0x1800, NdOp::INTRINSIC, {},
             {NdVar::cst(static_cast<unsigned>(Intrinsic::X87Wait), 2)});
      auto &Wait = Finally.Ops.back();
      if (Mutation == 1)
        Wait.Inputs[0].Offset = static_cast<unsigned>(Intrinsic::IntN);
      if (Mutation == 2)
        Wait.addInput(NdVar::reg(x86reg::RBP, 4));
      if (Mutation == 3)
        Wait.Output = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 4)
        Wait.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 5)
        Wait.Inputs[0].Size = 4;
      if (Mutation == 6)
        Wait.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
      emitOp(Finally, 0x1801, NdOp::RETURN, {}, {});
      Finally.EndAddr = 0x1802;
      Finally.InstructionBoundaries = {{0x1800, 1, 0, 1}, {0x1801, 1, 1, 1}};
      Finally.InstructionBoundaries.back().Control =
          LowInstructionControl::Return;
      Finally.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Return;
      const std::vector<RegistrationSEHControlContract> Controls{
          {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
           0x1800},
          {RegistrationSEHControlContract::Kind::NearReturn, 0x1801, 0x1802,
           InvalidVA},
          {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
           InvalidVA}};
      const std::vector<RegistrationCalleeStackContract> Stacks{
          {0x1800, 0, false}};
      const auto A = analyzeRegistrationStates(
          F, 0, 0, nullptr, nullptr, InspectReturningBody ? &Stacks : nullptr,
          nullptr, &Controls);
      EXPECT_EQ(A.Complete, Mutation == 0);
      if (Mutation == 0) {
        EXPECT_TRUE(A.CallbackStatesComplete);
        EXPECT_TRUE(A.SEHInvocationExitsComplete);
        EXPECT_EQ(A.SEHInvocationExits.size(), 1u);
        const auto State =
            std::find_if(A.Blocks.begin(), A.Blocks.end(), [&](const auto &B) {
              return B.BlockId == (InspectReturningBody ? 5 : 6);
            });
        ASSERT_NE(State, A.Blocks.end());
        EXPECT_TRUE(State->CanDispatch);
        EXPECT_EQ(Finally.Ops.front().Opcode, NdOp::INTRINSIC);
        EXPECT_EQ(Finally.Ops.front().Inputs[0].Offset,
                  static_cast<unsigned>(Intrinsic::X87Wait));
      }
    }
}

TEST(RegistrationState, SavedStackCopiesRetainTheirActualLoadOccurrence) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = invocationFrame(Mutation == 1 ? 2 : 0, false);
    auto &Finally = F.Blocks[6];
    auto &Load = Finally.Ops[1];
    Load.Output = NdVar::tmp(16, Mutation == 2 ? 8 : 4);
    LowOp Copy;
    Copy.Addr = Load.Addr;
    Copy.Seq = 2;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = NdVar::reg(x86reg::RSP, 4);
    Copy.NumInputs = 1;
    Copy.Inputs[0] = Load.Output;
    if (Mutation == 3)
      Copy.Inputs[0] = NdVar::tmp(24, 4);
    if (Mutation == 4)
      Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    if (Mutation == 5) {
      Copy.Opcode = NdOp::INT_ZEXT;
      Copy.Output.Size = 8;
    }
    if (Mutation == 6)
      Load.Seq = Copy.Seq;
    if (Mutation == 7)
      Load.Inputs[0] = NdVar::cst(uint32_t(-24), 4);
    Finally.Ops.insert(Finally.Ops.begin() + 2, Copy);
    Finally.InstructionBoundaries.front().OpCount += 1;
    for (size_t I = 1; I != Finally.InstructionBoundaries.size(); ++I)
      Finally.InstructionBoundaries[I].FirstOp += 1;
    const std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
         0x1800},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
         InvalidVA}};
    const std::vector<RegistrationLocalUnwindContract> LocalUnwinds{
        {0x2200, false}};
    const auto A = analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr,
                                             &LocalUnwinds, &Controls);
    EXPECT_EQ(A.Complete, Mutation == 0);
    EXPECT_EQ(A.SEHInvocationExitsComplete, Mutation == 0);
    if (Mutation == 0) {
      ASSERT_EQ(A.SEHInvocationExits.size(), 1u);
      const auto &Exit = A.SEHInvocationExits.front();
      EXPECT_EQ(Exit.Exit, RegistrationSEHInvocationExit::Kind::ParentJump);
      EXPECT_EQ(Exit.StackRestoreVA, 0x1800u);
      EXPECT_EQ(Exit.StackRestoreSeq, 2);
      EXPECT_EQ(Exit.StackOffset, -56);
      EXPECT_EQ(Exit.TargetVA, 0x1105u);
    } else
      EXPECT_TRUE(A.SEHInvocationExits.empty());
  }
}

TEST(RegistrationState, ReturningRuntimeCallCanPrecedeAFinallyReturn) {
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = invocationFrame(0, true);
    auto &Finally = F.Blocks[6];
    Finally.Ops.clear();
    Finally.InstructionBoundaries.clear();
    va_t Address = Finally.StartAddr;
    for (unsigned I = 0; I != 4; ++I) {
      const auto First = Finally.Ops.size();
      emitOp(Finally, Address, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Finally, Address, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(0, 4)});
      Finally.InstructionBoundaries.push_back({Address, 2, First, 2});
      Address += 2;
    }
    const va_t CallAddress = Address;
    emitOp(Finally, Address, NdOp::INDIR_CALL, {}, {NdVar::cst(0x2500, 4)});
    Finally.InstructionBoundaries.push_back(
        {Address, 6, Finally.Ops.size() - 1, 1});
    auto &Call = Finally.InstructionBoundaries.back();
    Call.Control = LowInstructionControl::Call;
    Call.ControlFlags =
        LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
    if (Mutation == 2)
      Call.ControlFlags |= LowInstructionControlFlag::Conditional;
    if (Mutation == 3)
      Call.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
    Address += 6;
    emitOp(Finally, Address, NdOp::RETURN, {}, {});
    Finally.InstructionBoundaries.push_back(
        {Address, 1, Finally.Ops.size() - 1, 1});
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    Finally.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    Finally.EndAddr = Address + 1;
    if (Mutation == 4)
      Finally.InstructionBoundaries.back().Address += 1;
    const std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1100, 0x1105,
         0x1800},
        {RegistrationSEHControlContract::Kind::NearReturn, Address, Address + 1,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1850, 0x1851,
         InvalidVA}};
    unsigned Invocations = 0;
    SEHJumpRuntime Runtime;
    Runtime.Raise = [=, &Invocations](const LowOp &Op, const FrameState &,
                                      size_t &) {
      ++Invocations;
      return Mutation != 1 && Op.Addr == CallAddress &&
             Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
             Op.Inputs[0] == NdVar::cst(0x2500, 4);
    };
    const auto States =
        RegistrationStateSolver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                &Controls, nullptr, nullptr, &Runtime)
            .run();
    if (!Mutation) {
      EXPECT_GT(Invocations, 0u);
      for (const auto &Diagnostic : States.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    }
    EXPECT_EQ(States.Complete, Mutation == 0);
    EXPECT_EQ(States.SEHInvocationExitsComplete, Mutation == 0);
    if (!Mutation)
      EXPECT_EQ(States.SEHInvocationExits.size(), 1u);
    else
      EXPECT_TRUE(States.SEHInvocationExits.empty());
  }
}

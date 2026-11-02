//===- RegistrationSEHExceptionTests.cpp - SEH runtime field ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tests invocation ownership of runtime exception fields.

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

LowFunc exceptionFilter(unsigned Mutation) {
  auto F = makeBranchingFrame();
  F.Blocks.resize(6);
  auto &Filter = F.Blocks[4];
  Filter.Id = 4;
  Filter.StartAddr = 0x1800;
  Filter.EndAddr = 0x1810;
  emitOp(Filter, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 8 ? -24 : -20), 4)});
  emitOp(Filter, 0x1800, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(0, 4)});
  if (Mutation == 2 || Mutation == 11) {
    emitOp(Filter, 0x1803, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Filter, 0x1803, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::reg(x86reg::RCX, Mutation == 11 ? 2 : 4)});
    emitOp(Filter, 0x1803, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::tmp(0, 4)});
  }
  if (Mutation == 3)
    emitOp(Filter, 0x1806, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RCX, 4), NdVar::cst(4, 4)});
  if (Mutation == 10)
    emitOp(Filter, 0x1806, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
  if (Mutation == 18 || Mutation == 19) {
    emitOp(Filter, 0x1808, NdOp::COPY, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RCX, 4)});
    if (Mutation == 19)
      emitOp(Filter, 0x1808, NdOp::INT_ADD, NdVar::tmp(8, 4),
             {NdVar::tmp(0, 2), NdVar::cst(0, 4)});
  }
  emitOp(Filter, 0x1808, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
         {Mutation == 18   ? NdVar::tmp(0, 2)
          : Mutation == 19 ? NdVar::tmp(8, 4)
                           : NdVar::reg(x86reg::RCX, 4)},
         Mutation == 6 ? NdMemoryAddressSpace::X86FS
                       : NdMemoryAddressSpace::Default);
  if (Mutation == 1 || Mutation == 4)
    emitOp(Filter, 0x180a, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RDX, 4), NdVar::cst(Mutation == 1 ? 4 : 8, 4)});
  if (Mutation == 7)
    emitOp(Filter, 0x180c, NdOp::STORE, {},
           {NdVar::reg(x86reg::RDX, 4), NdVar::cst(0, 4)});
  else
    emitOp(Filter, 0x180c, NdOp::LOAD,
           NdVar::reg(x86reg::RAX, Mutation == 5 ? 2 : 4),
           {NdVar::reg(x86reg::RDX, 4)});
  if (Mutation == 17) {
    emitOp(Filter, 0x180f, NdOp::STORE, {},
           {NdVar::reg(x86reg::RAX, 4), NdVar::cst(42, 4)});
    Filter.EndAddr = 0x1816;
  }
  emitOp(Filter, Filter.EndAddr - 1, NdOp::RETURN, {}, {});
  for (size_t Begin = 0; Begin != Filter.Ops.size();) {
    size_t End = Begin + 1;
    while (End != Filter.Ops.size() &&
           Filter.Ops[End].Addr == Filter.Ops[Begin].Addr)
      ++End;
    const va_t Next =
        End == Filter.Ops.size() ? Filter.EndAddr : Filter.Ops[End].Addr;
    Filter.InstructionBoundaries.push_back(
        {Filter.Ops[Begin].Addr, uint16_t(Next - Filter.Ops[Begin].Addr), Begin,
         End - Begin});
    Begin = End;
  }
  Filter.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  Filter.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
  auto &Handler = F.Blocks[5];
  Handler.Id = 5;
  Handler.StartAddr = 0x1900;
  Handler.EndAddr = 0x1901;
  if (Mutation == 9)
    F.OrdinaryModuleAnalysisRoots.insert(Filter.StartAddr);
  if (Mutation == 12)
    for (const auto &Insn : Filter.InstructionBoundaries)
      for (uint64_t Index = 0; Index != Insn.OpCount; ++Index)
        Filter.Ops[Insn.FirstOp + Index].Seq = int(Index);
  if (Mutation >= 13 && Mutation <= 16) {
    auto &Link =
        *std::find_if(Filter.InstructionBoundaries.begin(),
                      Filter.InstructionBoundaries.end(),
                      [](const auto &Insn) { return Insn.Address == 0x1808; });
    if (Mutation == 13)
      Link.OpCount = 0;
    if (Mutation == 14)
      Link.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
    if (Mutation == 15)
      Link.ControlFlags |= LowInstructionControlFlag::Conditional;
    if (Mutation == 16)
      for (auto &Op : Filter.Ops)
        if (Op.Addr == 0x1808)
          Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  }
  return F;
}

} // namespace

TEST(RegistrationState, SEHRuntimeFieldsBelongToTheSearchFilterInvocation) {
  const std::vector<RegistrationCalleeFrameContract> Callees;
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto A =
        analyzeRegistrationStates(exceptionFilter(Mutation), 0, 0, &Callees);
    const bool Valid = Mutation < 3 || Mutation == 12;
    EXPECT_EQ(A.Complete && A.CallbackStatesComplete && A.ImageReadsComplete,
              Valid);
    if (Valid) {
      EXPECT_TRUE(A.SEHExceptionAccessesComplete);
      ASSERT_EQ(A.SEHExceptionAccesses.size(), 3u);
      EXPECT_EQ(A.SEHExceptionAccesses[0].Field,
                RegistrationSEHExceptionAccess::Kind::PointersSlot);
      EXPECT_EQ(A.SEHExceptionAccesses[1].Field,
                RegistrationSEHExceptionAccess::Kind::RecordPointer);
      EXPECT_EQ(A.SEHExceptionAccesses[2].Field,
                Mutation == 1
                    ? RegistrationSEHExceptionAccess::Kind::ExceptionFlags
                    : RegistrationSEHExceptionAccess::Kind::ExceptionCode);
      for (const auto &Access : A.SEHExceptionAccesses)
        EXPECT_EQ(Access.FilterVA, 0x1800u);
    }
  }
}

TEST(RegistrationSEHException, EndedAndReenteredFiltersRetainOnlyPointerTaint) {
  using namespace neverd::registration_state;
  FrameState State;
  State.enterSEHFilter(0x1800, -20);
  const auto Pointers = State.load(-20, 4);
  ASSERT_TRUE(Pointers.SEHException);
  const auto Record = State.sehExceptionLoad(*Pointers.SEHException, 4);
  ASSERT_TRUE(Record && Record->SEHException);
  EXPECT_TRUE(State.sehExceptionLoad(*Record->SEHException, 4)->MayBeFrame);
  EXPECT_FALSE(State.sehExceptionLoad(*Record->SEHException, 2));
  auto Wrong = *Record->SEHException;
  Wrong.Filter = 0x1810;
  EXPECT_FALSE(State.sehExceptionLoad(Wrong, 4));
  Wrong = *Record->SEHException;
  Wrong.Offset = 8;
  EXPECT_FALSE(State.sehExceptionLoad(Wrong, 4));

  State.store(-28, 4, Pointers);
  State.storeEntry(-8, 4, *Record);
  State.storeCallback(-4, 4, *Record);
  State.Registers[0] = *Record;
  State.OtherRegisterBytes[0] = *Record;
  State.leaveCallback();
  for (const auto &Value :
       {State.load(-20, 4), State.load(-28, 4), State.loadEntry(-8, 4),
        State.Registers[0], State.OtherRegisterBytes[0]}) {
    EXPECT_TRUE(Value.MayBeFrame);
    EXPECT_FALSE(Value.SEHException);
  }
  EXPECT_TRUE(State.CallbackCells.empty());
  EXPECT_FALSE(State.sehExceptionLoad(*Record->SEHException, 4));

  State.enterSEHFilter(0x1800, -20);
  EXPECT_TRUE(State.load(-20, 4).SEHException);
  EXPECT_FALSE(State.load(-28, 4).SEHException);
  State.enterCallback(0x1800);
  EXPECT_FALSE(State.SEHFilterEntry);
  EXPECT_FALSE(State.sehExceptionLoad(*Record->SEHException, 4));
}

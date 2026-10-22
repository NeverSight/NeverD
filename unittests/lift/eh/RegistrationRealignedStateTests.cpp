//===- RegistrationRealignedStateTests.cpp - PE32 frame coordinates -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

LowFunc stackRealignedRegistrationFrame(bool WideAlignment = false) {
  LowFunc F;
  F.Entry = 0x1000;
  auto &EH = F.ExceptionMetadata.emplace();
  EH.CodeRange = {F.Entry, 0x103b};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 1;
  Cxx.UnwindMap.resize(1);
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::None;
  auto &Chain = EH.Registration.emplace();
  Chain.HandlerVA = 0x2000;
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -12;
  Chain.ChainInstallVA = 0x101e;
  Chain.RealignedFrame = RegistrationRealignedFrame{4, 0x1003, 16, 72, -72, 0};
  Chain.TryLevelStores = {{0x1024, 0x102c, 0}};
  F.Blocks.resize(4);
  const va_t Begins[] = {0x1000, 0x1024, 0x102c, 0x1037};
  const va_t Ends[] = {0x1024, 0x102c, 0x1037, 0x103b};
  for (unsigned I = 0; I != 4; ++I) {
    auto &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = Begins[I];
    B.EndAddr = Ends[I];
    if (I != 3)
      B.Succs = {int(I + 1)};
  }
  auto &Entry = F.Blocks[0];
  Entry.InstructionBoundaries = {
      {0x1000, 1}, {0x1001, 2}, {0x1003, 3}, {0x1006, 2}, {0x1008, 5},
      {0x100d, 6}, {0x1013, 1}, {0x1014, 3}, {0x1017, 1}, {0x1018, 1},
      {0x1019, 1}, {0x101a, 4}, {0x101e, 6}};
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto AX = NdVar::reg(x86reg::RAX, 4);
  const auto CX = NdVar::reg(x86reg::RCX, 4);
  auto Push = [&](va_t Address, NdVar Value) {
    emit(Entry, Address, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
    emit(Entry, Address, NdOp::STORE, {}, {SP, Value});
  };
  Push(0x1000, FP);
  emit(Entry, 0x1001, NdOp::COPY, FP, {SP});
  emit(Entry, 0x1003, NdOp::INT_AND, SP, {SP, NdVar::cst(0xfffffff0, 4)});
  Push(0x1006, NdVar::cst(0xffffffff, 4));
  Push(0x1008, NdVar::cst(0x2000, 4));
  emit(Entry, 0x100d, NdOp::LOAD, AX, {NdVar::cst(0, 4)},
       NdMemoryAddressSpace::X86FS);
  Push(0x1013, AX);
  emit(Entry, 0x1014, NdOp::INT_SUB, SP, {SP, NdVar::cst(48, 4)});
  Push(0x1017, NdVar::reg(x86reg::RBX, 4));
  Push(0x1018, NdVar::reg(x86reg::RSI, 4));
  Push(0x1019, NdVar::reg(x86reg::RDI, 4));
  emit(Entry, 0x101a, NdOp::INT_ADD, AX, {SP, NdVar::cst(60, 4)});
  emit(Entry, 0x101e, NdOp::STORE, {}, {NdVar::cst(0, 4), AX},
       NdMemoryAddressSpace::X86FS);
  auto &State = F.Blocks[1];
  State.InstructionBoundaries = {{0x1024, 8}};
  emit(State, 0x1024, NdOp::INT_ADD, NdVar::tmp(0, 4), {SP, NdVar::cst(68, 4)});
  emit(State, 0x1024, NdOp::STORE, {}, {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
  auto &Remove = F.Blocks[2];
  Remove.InstructionBoundaries = {{0x102c, 4}, {0x1030, 7}};
  emit(Remove, 0x102c, NdOp::INT_ADD, NdVar::tmp(0, 4),
       {SP, NdVar::cst(60, 4)});
  emit(Remove, 0x102c, NdOp::LOAD, CX, {NdVar::tmp(0, 4)});
  emit(Remove, 0x1030, NdOp::STORE, {}, {NdVar::cst(0, 4), CX},
       NdMemoryAddressSpace::X86FS);
  auto &Exit = F.Blocks[3];
  Exit.InstructionBoundaries = {{0x1037, 2}, {0x1039, 1}, {0x103a, 1}};
  Exit.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  emit(Exit, 0x1037, NdOp::COPY, SP, {FP});
  emit(Exit, 0x1039, NdOp::LOAD, FP, {SP});
  emit(Exit, 0x1039, NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
  emit(Exit, 0x103a, NdOp::RETURN, {}, {});
  if (WideAlignment) {
    for (auto &B : F.Blocks) {
      if (B.StartAddr >= 0x1006)
        B.StartAddr += 3;
      B.EndAddr += 3;
      for (auto &Boundary : B.InstructionBoundaries)
        if (Boundary.Address == 0x1003)
          Boundary.Size = 6;
        else if (Boundary.Address >= 0x1006)
          Boundary.Address += 3;
      for (auto &Op : B.Ops)
        if (Op.Addr >= 0x1006)
          Op.Addr += 3;
    }
    EH.CodeRange.End += 3;
    Chain.ChainInstallVA += 3;
    for (auto &Store : Chain.TryLevelStores) {
      Store.StoreVA += 3;
      Store.EndVA += 3;
    }
  }
  return F;
}

TEST(RegistrationRealignment, StackAlignedLocalsKeepEntryEBPIndependent) {
  for (bool Wide : {false, true}) {
    auto F = stackRealignedRegistrationFrame(Wide);
    const auto State = analyzeRegistrationStates(F);
    for (const auto &D : State.Diagnostics)
      SCOPED_TRACE(D);
    ASSERT_TRUE(State.Complete);
    EXPECT_TRUE(State.RegistrationLifetimeComplete);
    EXPECT_TRUE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, StackLayoutRejectsChangedNodeAndCoordinate) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = stackRealignedRegistrationFrame();
    auto &Layout = *F.ExceptionMetadata->Registration->RealignedFrame;
    if (Mutation == 0)
      Layout.BaseOffset -= 4;
    if (Mutation == 1)
      Layout.AllocationBytes += 4;
    if (Mutation == 2)
      Layout.SavedParentFrameOffset = -20;
    if (Mutation == 3)
      ++Layout.DefinitionVA;
    if (Mutation == 4)
      F.Blocks[0].InstructionBoundaries[2].Size = 2;
    if (Mutation == 10)
      F.ExceptionMetadata->Cxx->TryBlocks.emplace_back();
    for (auto &Op : F.Blocks[0].Ops) {
      if (Mutation == 5 && Op.Addr == 0x1003 && Op.Opcode == NdOp::INT_AND)
        Op.Inputs[1] = NdVar::cst(0xfffffff8, 4);
      if (Mutation == 6 && Op.Addr == 0x1014 && Op.Opcode == NdOp::INT_SUB)
        Op.Inputs[1] = NdVar::cst(44, 4);
      if (Mutation == 7 && Op.Addr == 0x101a && Op.Opcode == NdOp::INT_ADD)
        Op.Inputs[0] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 8 && Op.Addr == 0x1008 && Op.Opcode == NdOp::STORE)
        Op.Inputs[1] = NdVar::cst(0x2010, 4);
      if (Mutation == 9 && Op.Addr == 0x1006 && Op.Opcode == NdOp::STORE)
        Op.Inputs[1] = NdVar::cst(0, 4);
    }
    const auto State = analyzeRegistrationStates(F);
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, ReplaysTheEntryAndRuntimeFramesSeparately) {
  auto F = makeRealignedRegistrationFrame();
  auto State = analyzeRegistrationStates(F);
  for (const auto &D : State.Diagnostics)
    SCOPED_TRACE(D);
  ASSERT_TRUE(State.Complete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  EXPECT_TRUE(State.ChainOperationsComplete);
  ASSERT_EQ(State.Blocks.size(), 4u);
  EXPECT_EQ(State.Blocks.back().levels(), (std::vector<int32_t>{0}));
  EXPECT_TRUE(State.IncomingFrameAccessesComplete);
  EXPECT_TRUE(State.IncomingFrameAccesses.empty());
  const auto EntryFP = llvm::find_if(State.FrameValues, [](const auto &Value) {
    return Value.Address == 0x1001;
  });
  ASSERT_NE(EntryFP, State.FrameValues.end());
  EXPECT_FALSE(EntryFP->EstablishedFrameOffset);
}

TEST(RegistrationRealignment, RejectsChangedAllocationAlignmentAndRoot) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeRealignedRegistrationFrame();
    for (auto &Op : F.Blocks.front().Ops) {
      if (Mutation == 0 && Op.Opcode == NdOp::INT_AND && Op.Addr == 0x1006)
        Op.Inputs[1] = NdVar::cst(0xfffffff8, 4);
      if (Mutation == 1 && Op.Opcode == NdOp::INT_SUB && Op.Addr == 0x1009)
        Op.Inputs[1] = NdVar::cst(60, 4);
      if (Mutation == 2 && Op.Opcode == NdOp::COPY && Op.Addr == 0x100f)
        Op.Inputs[0] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 3 && Op.Opcode == NdOp::STORE && Op.Addr == 0x1011)
        Op.Inputs[1] = NdVar::reg(x86reg::RSI, 4);
      if (Mutation == 4 && Op.Opcode == NdOp::STORE && Op.Addr == 0x1017)
        Op.Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 5 && Op.Opcode == NdOp::STORE && Op.Addr == 0x101d)
        Op.Inputs[1] = NdVar::cst(0, 4);
    }
    const auto State = analyzeRegistrationStates(F);
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, RequiresDecodedEntryAndPublicationBoundaries) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeRealignedRegistrationFrame();
    if (Mutation == 0)
      F.Blocks[0].InstructionBoundaries[5].Size = 2;
    if (Mutation == 1)
      F.Blocks[0].InstructionBoundaries[5].Control =
          LowInstructionControl::Call;
    if (Mutation == 2)
      F.Blocks[1].InstructionBoundaries.back().Size = 7;
    if (Mutation == 3)
      F.ExceptionMetadata->Registration->RealignedFrame->BaseOffset = -64;
    const auto State = analyzeRegistrationStates(F);
    EXPECT_FALSE(State.Complete);
    EXPECT_FALSE(State.ChainOperationsComplete);
  }
}

TEST(RegistrationRealignment, EntryAddressCannotAliasTheAlignedStateSlot) {
  auto F = makeRealignedRegistrationFrame();
  for (auto &Op : F.Blocks[2].Ops)
    if (Op.Opcode == NdOp::INT_ADD)
      Op.Inputs[0] = NdVar::reg(x86reg::RBP, 4);
  const auto State = analyzeRegistrationStates(F);
  EXPECT_FALSE(State.Complete);
  EXPECT_FALSE(State.ChainOperationsComplete);
}

TEST(RegistrationRealignment, RejectsEntryLoadsBelowTheSavedRegisterArea) {
  auto F = makeRealignedRegistrationFrame();
  auto &Exit = F.Blocks.back();
  emit(Exit, 0x105a, NdOp::INT_ADD, NdVar::tmp(10, 4),
       {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emit(Exit, 0x105a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
       {NdVar::tmp(10, 4)});
  const auto State = analyzeRegistrationStates(F);
  EXPECT_FALSE(State.Complete);
  EXPECT_FALSE(State.IncomingFrameAccessesComplete);
  EXPECT_FALSE(State.ImageReadsComplete);
}

TEST(RegistrationRealignment, RejectsAlignedAccessesIntoTheEntryFrame) {
  for (unsigned Offset : {62u, 64u}) {
    SCOPED_TRACE(Offset);
    for (bool Store : {false, true}) {
      SCOPED_TRACE(Store);
      auto F = makeRealignedRegistrationFrame();
      auto &Exit = F.Blocks.back();
      emit(Exit, 0x105a, NdOp::INT_ADD, NdVar::tmp(10, 4),
           {NdVar::reg(x86reg::RSI, 4), NdVar::cst(Offset, 4)});
      if (Store)
        emit(Exit, 0x105a, NdOp::STORE, {},
             {NdVar::tmp(10, 4), NdVar::cst(0, 4)});
      else
        emit(Exit, 0x105a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
             {NdVar::tmp(10, 4)});
      const auto State = analyzeRegistrationStates(F);
      EXPECT_FALSE(State.Complete);
      EXPECT_FALSE(State.IncomingFrameAccessesComplete);
      EXPECT_FALSE(State.ImageReadsComplete);
    }
  }
}

} // namespace

//===- RegistrationRealignedStateTests.cpp - PE32 frame coordinates -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <utility>
#include <vector>

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

LowFunc stackRealignedCookieFrame(bool WideAlignment) {
  auto F = stackRealignedRegistrationFrame(WideAlignment);
  auto &EH = *F.ExceptionMetadata;
  auto &Chain = *EH.Registration;
  const va_t CookieLoad = 0x101a + (WideAlignment ? 3 : 0);
  for (auto &B : F.Blocks) {
    if (B.StartAddr >= CookieLoad)
      B.StartAddr += 8;
    B.EndAddr += 8;
    for (auto &Boundary : B.InstructionBoundaries)
      if (Boundary.Address >= CookieLoad)
        Boundary.Address += 8;
    for (auto &Op : B.Ops)
      if (Op.Addr >= CookieLoad)
        Op.Addr += 8;
  }
  EH.CodeRange.End += 8;
  Chain.ChainInstallVA += 8;
  Chain.TryLevelStores[0].StoreVA += 8;
  Chain.TryLevelStores[0].EndVA += 8;
  Chain.RealignedFrame->AllocationBytes += 4;
  Chain.RealignedFrame->BaseOffset -= 4;
  auto &Entry = F.Blocks[0];
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto AX = NdVar::reg(x86reg::RAX, 4);
  LowBlock Cookie;
  emit(Cookie, CookieLoad, NdOp::LOAD, AX, {NdVar::cst(0x4000, 4)});
  emit(Cookie, CookieLoad + 5, NdOp::INT_XOR, AX, {AX, SP});
  emit(Cookie, CookieLoad + 7, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
  emit(Cookie, CookieLoad + 7, NdOp::STORE, {}, {SP, AX});
  Entry.Ops.insert(Entry.Ops.end() - 2, Cookie.Ops.begin(), Cookie.Ops.end());
  Entry.InstructionBoundaries.insert(
      Entry.InstructionBoundaries.end() - 2,
      {{CookieLoad, 5}, {CookieLoad + 5, 2}, {CookieLoad + 7, 1}});
  for (size_t I = 0; I != Entry.Ops.size(); ++I)
    Entry.Ops[I].Seq = I;
  Entry.Ops[Entry.Ops.size() - 2].Inputs[1] = NdVar::cst(64, 4);
  F.Blocks[1].Ops.front().Inputs[1] = NdVar::cst(72, 4);
  F.Blocks[2].Ops.front().Inputs[1] = NdVar::cst(64, 4);

  // The explicit check precedes chain removal; the entry EBP remains a
  // different coordinate even while the aligned cookie is live.
  const va_t CheckBegin = F.Blocks[1].EndAddr;
  for (size_t I : {2u, 3u}) {
    auto &B = F.Blocks[I];
    B.StartAddr += 14;
    B.EndAddr += 14;
    for (auto &Boundary : B.InstructionBoundaries)
      Boundary.Address += 14;
    for (auto &Op : B.Ops)
      Op.Addr += 14;
  }
  EH.CodeRange.End += 14;
  F.Blocks[1].Succs = {4};
  LowBlock Check;
  Check.Id = 4;
  Check.StartAddr = CheckBegin;
  Check.EndAddr = CheckBegin + 14;
  Check.Succs = {2};
  Check.InstructionBoundaries = {{CheckBegin, 3},
                                 {CheckBegin + 3, 4},
                                 {CheckBegin + 7, 2},
                                 {CheckBegin + 9, 5}};
  Check.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  const auto CX = NdVar::reg(x86reg::RCX, 4);
  const auto DX = NdVar::reg(x86reg::RDX, 4);
  emit(Check, CheckBegin, NdOp::LOAD, CX, {SP});
  emit(Check, CheckBegin + 3, NdOp::INT_ADD, DX, {SP, NdVar::cst(4, 4)});
  emit(Check, CheckBegin + 7, NdOp::INT_XOR, CX, {CX, DX});
  emit(Check, CheckBegin + 9, NdOp::CALL, AX, {NdVar::cst(0x5000, 4)});
  F.Blocks.push_back(std::move(Check));
  return F;
}

RegistrationStateAnalysis analyzeStackCookieFrame(const LowFunc &F,
                                                  int32_t Slot = -64,
                                                  int32_t XOR = -60) {
  coff_loader::X86CxxHandlerABI Handler;
  Handler.SecurityCookieVA = 0x4000;
  Handler.CookieGuards.push_back({Slot, XOR, 0x3000, 0x5000, 0x5018});
  const std::vector<RegistrationCalleeFrameContract> Callees;
  const std::vector<RegistrationCleanupFrameContract> Cleanups;
  return analyzeRegistrationStates(F, 0x4000, 0, &Callees, &Cleanups, nullptr,
                                   nullptr, nullptr, &Handler);
}

TEST(RegistrationRealignment, EntryRegisterSavesPreserveTheAlignedCookie) {
  for (bool Wide : {false, true}) {
    SCOPED_TRACE(Wide);
    const auto State = analyzeStackCookieFrame(stackRealignedCookieFrame(Wide));
    for (const auto &D : State.Diagnostics)
      SCOPED_TRACE(D);
    ASSERT_TRUE(State.Complete);
    EXPECT_TRUE(State.ChainOperationsComplete);
    EXPECT_TRUE(State.CallFrameEffectsComplete);
    EXPECT_TRUE(State.CleanupFrameEffectsComplete);
    EXPECT_TRUE(State.RuntimeObjectAccessesComplete);
    EXPECT_TRUE(State.ImageReadsComplete);
    ASSERT_TRUE(State.SecurityCookiesComplete);
    ASSERT_EQ(State.CxxCookieConditions.size(), 1u);
    EXPECT_EQ(State.CxxCookieConditions[0].FrameOffset, -76);
    EXPECT_EQ(State.CxxCookieConditions[0].XORFrameOffset, -72);
    EXPECT_EQ(State.CookieChecks.size(), 1u);
  }
}

TEST(RegistrationRealignment, EntrySavesAlsoPreserveTheESIBasedCookie) {
  auto F = makeRealignedRegistrationFrame();
  auto &Install = F.Blocks[1];
  LowBlock Cookie;
  const auto Value = NdVar::tmp(80, 4);
  const auto Address = NdVar::tmp(81, 4);
  const auto Local = NdVar::reg(x86reg::RSI, 4);
  emit(Cookie, 0x103e, NdOp::LOAD, Value, {NdVar::cst(0x4000, 4)});
  emit(Cookie, 0x103e, NdOp::INT_XOR, Value, {Value, Local});
  emit(Cookie, 0x103e, NdOp::INT_ADD, Address, {Local, NdVar::cst(36, 4)});
  emit(Cookie, 0x103e, NdOp::STORE, {}, {Address, Value});
  Install.Ops.insert(Install.Ops.end() - 1, Cookie.Ops.begin(),
                     Cookie.Ops.end());
  for (size_t I = 0; I != Install.Ops.size(); ++I)
    Install.Ops[I].Seq = I;
  const auto State = analyzeStackCookieFrame(F, -12, -48);
  for (const auto &D : State.Diagnostics)
    SCOPED_TRACE(D);
  ASSERT_TRUE(State.Complete);
  EXPECT_TRUE(State.CallFrameEffectsComplete);
  EXPECT_TRUE(State.CleanupFrameEffectsComplete);
  ASSERT_TRUE(State.SecurityCookiesComplete);
  ASSERT_EQ(State.CxxCookieConditions.size(), 1u);
  EXPECT_EQ(State.CxxCookieConditions[0].FrameOffset, -24);
  EXPECT_EQ(State.CxxCookieConditions[0].XORFrameOffset, -60);
}

TEST(RegistrationRealignment,
     AlignedCookiesStillRequireDisjointCurrentAccesses) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = stackRealignedCookieFrame(false);
    auto &Entry = F.Blocks[0];
    auto &Body = F.Blocks[1];
    if (Mutation < 3) {
      const auto Address = NdVar::tmp(80, 4);
      const auto FP = NdVar::reg(x86reg::RBP, 4);
      const auto Base = Mutation == 2 ? NdVar::reg(x86reg::RDX, 4) : FP;
      emit(Body, Body.StartAddr, NdOp::INT_ADD, Address,
           {Base, NdVar::cst(Mutation == 0 ? uint32_t(-4) : 0, 4)});
      if (Mutation == 0)
        emit(Body, Body.StartAddr, NdOp::LOAD, NdVar::tmp(81, 4), {Address});
      else
        emit(Body, Body.StartAddr, NdOp::STORE, {},
             {Address, NdVar::cst(0, 4)});
    }
    for (auto &Op : Entry.Ops) {
      if (Mutation == 3 && Op.Addr == 0x101f)
        Op.Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 4 && Op.Addr == 0x1021 && Op.Opcode == NdOp::STORE)
        Op.Inputs[1].Size = 1;
      if (Mutation == 7 && Op.Opcode == NdOp::INT_AND)
        Op.Inputs[1] = NdVar::cst(0xfffffff8, 4);
    }
    const auto State = analyzeStackCookieFrame(F, Mutation == 5 ? -63 : -64,
                                               Mutation == 6 ? -56 : -60);
    EXPECT_FALSE(State.SecurityCookiesComplete);
    EXPECT_TRUE(State.CxxCookieConditions.empty());
    EXPECT_TRUE(State.CookieChecks.empty());
  }
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

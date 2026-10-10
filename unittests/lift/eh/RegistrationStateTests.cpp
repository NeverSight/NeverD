//===- RegistrationStateTests.cpp - Registration-state CFG proofs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <tuple>

namespace {

using namespace neverd;
using namespace neverd::registration_test;

TEST(RegistrationState, DeadFrameValuesDoNotAccumulateAcrossJoins) {
  using registration_state::FrameState;
  using registration_state::FrameValue;
  for (unsigned Space = 0; Space != 3; ++Space) {
    for (bool OpaqueCall : {false, true}) {
      SCOPED_TRACE(Space);
      SCOPED_TRACE(OpaqueCall);
      FrameState State;
      auto Store = [&](FrameState &S, int32_t Offset, uint16_t Width,
                       FrameValue Value) {
        if (Space == 0)
          S.store(Offset, Width, Value);
        else if (Space == 1)
          S.storeEntry(Offset, Width, Value);
        else
          S.storeCallback(Offset, Width, Value);
      };
      auto Load = [&](int32_t Offset, uint16_t Width) {
        return Space == 0   ? State.load(Offset, Width)
               : Space == 1 ? State.loadEntry(Offset, Width)
                            : State.loadCallback(Offset, Width);
      };
      // Every join invalidates a distinct scalar spill. The surviving frame
      // pointer is a may fact: neither a missing predecessor store nor a
      // partial overwrite may clear its provenance.
      Store(State, -8, 4, FrameValue::frame(-32));
      for (int32_t I = 0; I != 1024; ++I) {
        const int32_t Offset = -16 - 4 * I;
        Store(State, Offset, 4, FrameValue::constant(uint32_t(I)));
        if (OpaqueCall)
          State.forgetCellValues();
        else
          State.merge(FrameState{});
        EXPECT_EQ(Load(Offset, 4), FrameValue{});
        EXPECT_TRUE(Load(-8, 4).MayBeFrame);
        EXPECT_FALSE(Load(-8, 4).Offset);
        EXPECT_EQ(State.Cells.size() + State.EntryCells.size() +
                      State.CallbackCells.size(),
                  1u);
      }
      Store(State, -7, 1, FrameValue::constant(0));
      EXPECT_TRUE(Load(-8, 4).MayBeFrame);
      EXPECT_TRUE(Load(-6, 1).MayBeFrame);
      EXPECT_FALSE(Load(-4, 4).MayBeFrame);
      Store(State, -8, 4, FrameValue::constant(17));
      EXPECT_EQ(Load(-8, 4), FrameValue::constant(17));
      FrameState OtherPath;
      Store(OtherPath, -7, 4, FrameValue::frame(-32));
      State.merge(OtherPath);
      // The invalidated exact scalar cell must not hide overlapping pointer
      // bytes that reach this load through the other predecessor.
      EXPECT_TRUE(Load(-8, 4).MayBeFrame);
      EXPECT_FALSE(Load(-8, 4).Constant);
    }
  }
}

LowFunc makeCookieFrame(bool GS = false) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.ScopeTableVA = 0x3000;
  Chain.EHCookieOffset = -28;
  Chain.GSCookieOffset = GS ? -36 : -2;
  Chain.GSCookieXOROffset = GS ? 4 : 0;
  Chain.HasSecurityCookies = GS;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-2), 4);
  auto &Entry = F.Blocks.front();
  auto Install = Entry.Ops.back();
  Entry.Ops.pop_back();
  emitOp(Entry, 0x1000, NdOp::COPY, NdVar::tmp(20, 4),
         {NdVar::reg(x86reg::RSP, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(24, 4)});
  emitOp(Entry, 0x1000, NdOp::LOAD, NdVar::tmp(21, 4), {NdVar::cst(0x4000, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(22, 4),
         {NdVar::tmp(21, 4), NdVar::cst(0x3000, 4)});
  auto Store = [&](int32_t Slot, NdVar Value) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(24, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Slot), 4)});
    emitOp(Entry, 0x1000, NdOp::STORE, {}, {NdVar::tmp(24, 4), Value});
  };
  Store(-8, NdVar::tmp(22, 4));
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(23, 4),
         {NdVar::tmp(21, 4), NdVar::reg(x86reg::RBP, 4)});
  Store(-28, NdVar::tmp(23, 4));
  if (GS) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(25, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(4, 4)});
    emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(26, 4),
           {NdVar::tmp(21, 4), NdVar::tmp(25, 4)});
    Store(-36, NdVar::tmp(26, 4));
  }
  Install.Seq = Entry.Ops.size();
  Install.Inputs[1] = NdVar::tmp(20, 4);
  Entry.Ops.push_back(Install);
  auto &Exit = F.Blocks.back();
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(30, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::tmp(31, 4),
         {NdVar::tmp(30, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
         {NdVar::cst(0, 4), NdVar::tmp(31, 4)}, NdMemoryAddressSpace::X86FS);
  return F;
}

TEST(RegistrationState, EH4CookiesUseTheAuthenticatedImageAndRuntimeFrame) {
  for (bool GS : {false, true}) {
    auto F = makeCookieFrame(GS);
    auto Result = analyzeRegistrationStates(F, 0x4000);
    ASSERT_TRUE(Result.Complete);
    ASSERT_TRUE(Result.RegistrationLifetimeComplete);
    EXPECT_TRUE(Result.SecurityCookiesComplete);
    EXPECT_EQ(Result.SecurityCookieVA, 0x4000u);
    EXPECT_FALSE(analyzeRegistrationStates(F).SecurityCookiesComplete);
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4004).SecurityCookiesComplete);
  }
}

LowFunc makeCheckedCookieExit() {
  auto F = makeCookieFrame(true);
  auto &Exit = F.Blocks.back();
  auto Unlink = std::move(Exit.Ops);
  Exit.Ops.clear();
  Exit.EndAddr = Exit.StartAddr + 12;
  Exit.InstructionBoundaries = {{Exit.StartAddr, 5}, {Exit.StartAddr + 5, 7}};
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(40, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-36), 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(40, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(41, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(4, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::INT_XOR, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RCX, 4), NdVar::tmp(41, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x5000, 4)});
  for (auto Op : Unlink) {
    Op.Addr += 5;
    Op.Seq = Exit.Ops.size();
    Exit.Ops.push_back(Op);
  }
  return F;
}

TEST(RegistrationState, EH4ExitCheckRequiresTheExactDecodedCookie) {
  auto F = makeCheckedCookieExit();
  auto Result = analyzeRegistrationStates(F, 0x4000, 0x5000);
  ASSERT_TRUE(Result.SecurityCookiesComplete);
  ASSERT_EQ(Result.CookieChecks.size(), 1u);
  EXPECT_EQ(Result.CookieCheckVA, 0x5000u);
  EXPECT_NE(Result.cookieCheck(0x1030, 4), nullptr);
  EXPECT_EQ(Result.cookieCheck(0x1030, 3), nullptr);
  EXPECT_EQ(Result.CookieChecks.front().EndAddress, 0x1035u);
  EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Changed = F;
    auto &Exit = Changed.Blocks.back();
    if (Mutation == 0)
      Exit.Ops[2].Inputs[1] = NdVar::cst(8, 4);
    if (Mutation == 1)
      Exit.Ops[1].Output.Size = 1;
    if (Mutation == 2)
      Exit.Ops[3].Opcode = NdOp::INT_ADD;
    if (Mutation == 3)
      Exit.Ops[4].Inputs[0] = NdVar::cst(0x5010, 4);
    if (Mutation == 4)
      Exit.Ops[4].Seq = -1;
    if (Mutation == 5)
      Exit.InstructionBoundaries.erase(Exit.InstructionBoundaries.begin());
    if (Mutation == 6)
      Exit.Ops[4].Inputs[0].Size = 1;
    if (Mutation == 7)
      Exit.Ops[3].Output = NdVar::reg(x86reg::RDX, 4);
    auto Invalid = analyzeRegistrationStates(Changed, 0x4000, 0x5000);
    EXPECT_FALSE(Invalid.SecurityCookiesComplete) << Mutation;
    EXPECT_TRUE(Invalid.CookieChecks.empty()) << Mutation;
  }
}

TEST(RegistrationState, EH4CookieInitializationCannotBeNarrowOrUnencoded) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto F = makeCookieFrame();
    auto &Ops = F.Blocks.front().Ops;
    if (Mutation == 0)
      F.ExceptionMetadata->Registration->EHCookieXOROffset = 4;
    if (Mutation == 1)
      F.ExceptionMetadata->Registration->EHCookieOffset = -20;
    for (auto &Op : Ops) {
      if (Mutation == 2 && Op.Output == NdVar::tmp(22, 4))
        Op.Inputs[1] = NdVar::cst(0x3004, 4);
      if (Mutation == 3 && Op.Output == NdVar::tmp(21, 4))
        Op.Output.Size = 1;
      if (Mutation == 4 && Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.Inputs[1] == NdVar::tmp(23, 4))
        Op.Inputs[1].Size = 1;
    }
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete)
        << Mutation;
  }
}

TEST(RegistrationState, EH4CookiesAndScopeEncodingRemainRuntimePrivate) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto F = makeCookieFrame();
    auto &Block = F.Blocks[1];
    NdVar Address = NdVar::cst(Mutation == 3 ? 0x4000 : 0x3008, 4);
    if (Mutation < 3) {
      emitOp(Block, Block.StartAddr, NdOp::INT_ADD, NdVar::tmp(40, 4),
             {NdVar::reg(x86reg::RBP, 4),
              NdVar::cst(uint32_t(Mutation == 0 ? -28 : -8), 4)});
      Address = NdVar::tmp(40, 4);
    }
    if (Mutation == 0)
      emitOp(Block, Block.StartAddr, NdOp::LOAD, NdVar::tmp(41, 4), {Address});
    else
      emitOp(Block, Block.StartAddr, NdOp::STORE, {},
             {Address, NdVar::cst(0, 4)});
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete)
        << Mutation;
  }
}

TEST(RegistrationState, IncomingCallerFrameUsesTheFinalCFGValue) {
  for (bool Agree : {false, true}) {
    LowFunc F = makeBranchingFrame();
    for (unsigned I = 1; I != 3; ++I)
      emitOp(F.Blocks[I], F.Blocks[I].StartAddr, NdOp::INT_ADD,
             NdVar::reg(x86reg::RAX, 4),
             {NdVar::reg(x86reg::RBP, 4),
              NdVar::cst(I == 1 || Agree ? 8 : 12, 4)});
    emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::tmp(9, 4),
           {NdVar::reg(x86reg::RAX, 4)});
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    EXPECT_EQ(Result.IncomingFrameAccessesComplete, Agree);
    if (Agree) {
      ASSERT_EQ(Result.IncomingFrameAccesses.size(), 1u);
      EXPECT_EQ(Result.IncomingFrameAccesses.front().Offset, 8);
      EXPECT_EQ(Result.IncomingFrameAccesses.front().Width, 4u);
      EXPECT_FALSE(Result.IncomingFrameAccesses.front().Write);
    } else
      EXPECT_TRUE(Result.IncomingFrameAccesses.empty());
  }
}

TEST(RegistrationState, RetainsBothStatesAtJoin) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 4u);
  EXPECT_EQ(Result.Blocks[3].Levels, (std::vector<int32_t>{-1, 0}));
  EXPECT_FALSE(registrationRangesWhere(
      Result, [](int32_t Level) { return Level == 0; }));
}

TEST(RegistrationState, ReplaysLoopBackedges) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[3].Succs = {1};
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].Levels.empty());
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1, 0}));
}

TEST(RegistrationState, DoesNotSeedBeforeTheRegistrationIsInstalled) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].Levels.empty());
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1}));
}

TEST(RegistrationState, MissingInstallationBoundaryCannotAuthorizeTheSeed) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->ChainInstallVA++;
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, InstallationBytesNeedTheActualLiftedChainWrite) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Ops.pop_back();
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, AnUnknownHandlerDoesNotAcquireSEHSemantics) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Personality = ExceptionPersonality::Unknown;
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, DoesNotApplyStoresFromUnreachableBlocks) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[2].Levels.empty());
  EXPECT_EQ(Result.Blocks[3].Levels, (std::vector<int32_t>{0}));
}

TEST(RegistrationState, KeepsTheIncomingLevelUntilTheStoreRetires) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1}));
}

TEST(RegistrationState, InvalidInstructionBoundaryCannotAuthorizeAState) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelStores[0].StoreVA++;
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 4u);
  EXPECT_TRUE(Result.Blocks[3].Unknown);
}

TEST(RegistrationState, InvalidStateCannotDisappearAtALaterReset) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelStores[0].Level = 9;
  F.ExceptionMetadata->Registration->TryLevelStores.push_back(
      {0x1030, 0x1037, -1});
  addSlotStore(F.Blocks[3], -1);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(registrationRangesWhere(
      Result, [](int32_t Level) { return Level >= 0; }));
}

TEST(RegistrationState, APartialWriteCannotKeepThePreviousState) {
  LowFunc F = makeBranchingFrame();
  addSlotStore(F.Blocks[3], 1, 1);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

LowFunc makeNarrowCxxStateFrame(uint8_t Width, int32_t First, int32_t Other,
                                int32_t Immediate) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  auto &Chain = *EH.Registration;
  Chain.RegistrationOffset = -12;
  Chain.Scopes.clear();
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 258;
  Cxx.UnwindMap.resize(Cxx.MaxState);
  for (auto &Action : Cxx.UnwindMap)
    Action.Kind = CxxUnwindAction::ActionKind::None;
  F.Blocks[0].Ops[2].Inputs[1] = NdVar::cst(12, 4);
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(First), 4);
  F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(uint32_t(Other), 4);
  Chain.TryLevelStores[0].Level = First;
  Chain.TryLevelStores[1].Level = Other;
  auto &Write = F.Blocks[3];
  Write.EndAddr = Write.StartAddr + 4;
  Write.InstructionBoundaries = {{Write.StartAddr, 4}};
  addSlotStore(Write, Immediate, Width);
  Chain.TryLevelStores.push_back(
      {Write.StartAddr, Write.EndAddr, Immediate, Width});
  Write.Succs = {4};
  LowBlock After;
  After.Id = 4;
  After.StartAddr = 0x1040;
  After.EndAddr = 0x1041;
  F.Blocks.push_back(std::move(After));
  return F;
}

TEST(RegistrationState, NarrowCxxStoresPreserveEveryReachingHighByte) {
  for (const auto &[Width, First, Other, Immediate, Expected] :
       {std::tuple{uint8_t(1), 256, 256, 1, std::vector<int32_t>{257}},
        std::tuple{uint8_t(1), 0, 256, 1, std::vector<int32_t>{1, 257}},
        std::tuple{uint8_t(1), -1, -1, 255, std::vector<int32_t>{-1}},
        std::tuple{uint8_t(2), 0, 256, 257, std::vector<int32_t>{257}}}) {
    auto F = makeNarrowCxxStateFrame(Width, First, Other, Immediate);
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete) << unsigned(Width) << ' ' << First;
    ASSERT_EQ(Result.Blocks.size(), 5u);
    EXPECT_EQ(Result.Blocks.back().Levels, Expected);
    EXPECT_FALSE(Result.Blocks.back().Unknown);
  }
}

TEST(RegistrationState, NarrowCxxStoresNeedThePriorWholeStateAndExactWidth) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto F = makeNarrowCxxStateFrame(1, 0, 0, 1);
    auto &Store = F.ExceptionMetadata->Registration->TryLevelStores.back();
    if (Mutation == 0) {
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-1), 4);
      F.ExceptionMetadata->Registration->TryLevelStores[0].Level = -1;
    }
    if (Mutation == 1)
      Store.Width = 4;
    if (Mutation == 2)
      F.Blocks[3].Ops.back().Inputs[1] = NdVar::cst(2, 1);
    if (Mutation == 3) {
      Store.Level = 256;
      F.Blocks[3].Ops.back().Inputs[1] = NdVar::cst(256, 1);
    }
    if (Mutation == 4)
      F.Blocks[3].Ops.front().Inputs[1] = NdVar::cst(uint32_t(-3), 4);
    if (Mutation == 5) {
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
    }
    const auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete) << Mutation;
    ASSERT_EQ(Result.Blocks.size(), 5u);
    EXPECT_TRUE(Result.Blocks.back().Unknown) << Mutation;
  }
}

namespace {
LowFunc makeCxxObjectCall() {
  auto F = makeCxxCatchContinuation();
  LowBlock Setup;
  emitOp(Setup, 0x1013, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1013, NdOp::STORE, {},
         {NdVar::reg(x86reg::RCX, 4), NdVar::cst(7, 4)});
  auto &Body = F.Blocks[1];
  auto BeforeState =
      std::find_if(Body.Ops.begin(), Body.Ops.end(),
                   [](const auto &Op) { return Op.Addr == 0x1016; });
  Body.Ops.insert(BeforeState, Setup.Ops.begin(), Setup.Ops.end());
  for (size_t I = 0; I < Body.Ops.size(); ++I)
    Body.Ops[I].Seq = I;
  LowBlock Call;
  Call.Id = 7;
  Call.StartAddr = 0x1100;
  Call.EndAddr = 0x1105;
  Call.Succs = {2};
  LowInstructionBoundary Boundary;
  Boundary.Address = Call.StartAddr;
  Boundary.Size = 5;
  Boundary.Control = LowInstructionControl::Call;
  Call.InstructionBoundaries.push_back(Boundary);
  emitOp(Call, 0x1100, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
  Body.Succs = {7};
  F.Blocks.push_back(std::move(Call));
  return F;
}

RegistrationCalleeFrameContract objectLeafContract() {
  RegistrationCalleeFrameContract C;
  C.Target = 0x2100;
  C.ECXReads = {{0, 4}};
  C.ImageReads = {{0x3000, 0x3004}};
  C.ImageWrites = {{0x3000, 0x3004}};
  return C;
}
} // namespace

TEST(RegistrationState, ProjectsTheInitializedObjectAtTheExactSourceCall) {
  const auto F = makeCxxObjectCall();
  const std::vector Contracts{objectLeafContract()};
  const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
  EXPECT_TRUE(A.Complete);
  EXPECT_TRUE(A.RegistrationLifetimeComplete);
  EXPECT_TRUE(A.CallFrameEffectsComplete);
  EXPECT_TRUE(A.ImageReadsComplete);
  ASSERT_EQ(A.CallFrameEffects.size(), 1u);
  const auto *Call = A.callFrameEffect(0x1100, 0);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->EndAddress, 0x1105u);
  EXPECT_EQ(Call->Target, 0x2100u);
  EXPECT_EQ(Call->ECXFrameOffset, -24);
  EXPECT_EQ(Call->FrameReads,
            (std::vector<RegistrationObjectExtent>{{-24, -20}}));
  EXPECT_FALSE(Call->DoesNotReturn);
  EXPECT_EQ(A.callFrameEffect(0x1100, 1), nullptr);
  EXPECT_EQ(A.callFrameEffect(0x1101, 0), nullptr);
  ASSERT_EQ(A.ImageReads.size(), 1u);
  EXPECT_EQ(A.ImageReads.front().Begin, 0x3000u);
}

TEST(RegistrationState, StackCleanupDoesNotGrantMemoryBorrowAuthority) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeBranchingFrame();
    LowBlock Call;
    Call.Id = 4;
    Call.StartAddr = 0x1100;
    Call.EndAddr = 0x1109;
    Call.Succs = {1, 2};
    Call.InstructionBoundaries = {{0x1100, 5}, {0x1105, 3}, {0x1108, 1}};
    Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
    emitOp(Call, 0x1100, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    emitOp(Call, 0x1105, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1108, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
    F.Blocks[0].Succs = {4};
    F.Blocks.push_back(std::move(Call));
    std::vector<RegistrationCalleeStackContract> Stacks{{0x2100, 0, false}};
    if (Mutation == 1)
      Stacks.clear();
    if (Mutation == 2)
      Stacks[0].Indirect = true;
    if (Mutation == 3)
      Stacks[0].Target = 0x2200;
    if (Mutation == 4)
      Stacks[0].StackPopBytes = 4;
    if (Mutation == 5)
      Stacks.push_back(Stacks[0]);
    const auto A =
        analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, &Stacks);
    EXPECT_EQ(A.Complete, Mutation == 0);
    EXPECT_FALSE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.CallFrameEffects.empty());
    EXPECT_TRUE(A.CalleeContracts.empty());
  }
}

namespace {
LowFunc makeCxxObjectCleanup() {
  auto F = makeCxxObjectCall();
  F.ExceptionMetadata->Cxx->UnwindMap[0] = {
      -1, 0x2200, CxxUnwindAction::ActionKind::Direct};
  return F;
}

RegistrationCleanupFrameContract objectCleanupContract() {
  RegistrationCleanupFrameContract C;
  C.ActionState = 0;
  C.RelayTarget = 0x2200;
  C.ObjectFrameOffset = -24;
  C.Leaf = objectLeafContract();
  return C;
}
} // namespace

TEST(RegistrationState, ProjectsInitializedCleanupObjectsAtEveryDispatch) {
  auto F = makeCxxObjectCleanup();
  const std::vector Calls{objectLeafContract()};
  const std::vector Cleanups{objectCleanupContract()};
  const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
  ASSERT_TRUE(A.Complete);
  ASSERT_TRUE(A.CleanupFrameEffectsComplete);
  ASSERT_FALSE(A.CleanupFrameEffects.empty());
  for (const auto &Effect : A.CleanupFrameEffects) {
    EXPECT_EQ(Effect.ActionState, 0u);
    EXPECT_EQ(Effect.CleanupIndex, 0u);
    EXPECT_EQ(Effect.DispatchLevel, 0);
    EXPECT_EQ(Effect.StackOffset, -28);
    EXPECT_EQ(Effect.FrameReads,
              (std::vector<RegistrationObjectExtent>{{-24, -20}}));
    EXPECT_TRUE(Effect.FrameWrites.empty());
    ASSERT_GE(Effect.BlockId, 0);
    EXPECT_EQ(Effect.Range.Begin, F.Blocks[Effect.BlockId].StartAddr);
  }
}

TEST(RegistrationState, RejectsUnprovedCleanupObjectsAndContracts) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxObjectCleanup();
    std::vector Calls{objectLeafContract()};
    std::vector Cleanups{objectCleanupContract()};
    auto &C = Cleanups[0];
    switch (Mutation) {
    case 0:
      Cleanups.clear();
      break;
    case 1:
      C.RelayTarget = 0x2201;
      break;
    case 2:
      C.ActionState = 1;
      break;
    case 3:
      C.ObjectFrameOffset = -16; // SavedESP.
      break;
    case 4:
      C.ObjectFrameOffset = -12; // The registration link.
      break;
    case 5:
      C.ObjectFrameOffset = -32; // Below the allocated parent stack.
      break;
    case 6:
      C.ObjectFrameOffset = 0; // Saved EBP.
      break;
    case 7:
      C.Leaf.StackPopBytes = 4;
      break;
    case 8:
      C.Leaf.ECXReads[0].End = 8; // Four bytes remain uninitialized.
      break;
    case 9:
      C.Leaf.CallerPCWrites = {{0x3100, 0x3104}};
      break;
    case 10:
      for (auto &Op : F.Blocks[1].Ops)
        if (Op.Opcode == NdOp::STORE && Op.Inputs[0].isReg() &&
            Op.Inputs[0].Offset == x86reg::RCX)
          Op.Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      break;
    case 11:
      C.Leaf.ECXReads = {{4, 4}};
      break;
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
    EXPECT_FALSE(A.CleanupFrameEffectsComplete);
    EXPECT_TRUE(A.CleanupFrameEffects.empty());
  }
}

TEST(RegistrationState, CleanupInitializationMustPrecedeStateActivation) {
  for (bool BothInitialized : {false, true}) {
    auto F = makeCxxObjectCleanup();
    const std::vector Calls{objectLeafContract()};
    const std::vector Cleanups{objectCleanupContract()};
    auto &Body = F.Blocks[1];
    Body.Ops.erase(std::remove_if(Body.Ops.begin(), Body.Ops.end(),
                                  [](const auto &Op) {
                                    return Op.Addr == 0x1016 ||
                                           (Op.Opcode == NdOp::STORE &&
                                            Op.Inputs[0].isReg() &&
                                            Op.Inputs[0].Offset == x86reg::RCX);
                                  }),
                   Body.Ops.end());
    Body.Succs = {8, 9};
    for (int Id : {8, 9}) {
      LowBlock B;
      B.Id = Id;
      B.StartAddr = 0x1120 + (Id - 8) * 8;
      B.EndAddr = B.StartAddr + 7;
      B.InstructionBoundaries = {{B.StartAddr, 7}};
      B.Succs = {10};
      if (Id == 8 || BothInitialized)
        emitOp(B, B.StartAddr, NdOp::STORE, {},
               {NdVar::reg(x86reg::RCX, 4), NdVar::cst(7, 4)});
      F.Blocks.push_back(std::move(B));
    }
    LowBlock Activate;
    Activate.Id = 10;
    Activate.StartAddr = 0x1140;
    Activate.EndAddr = 0x1147;
    Activate.InstructionBoundaries = {{0x1140, 7}};
    Activate.Succs = {7};
    addSlotStore(Activate, 0);
    F.Blocks.push_back(Activate);
    auto &Stores = F.ExceptionMetadata->Registration->TryLevelStores;
    for (auto &Store : Stores)
      if (Store.StoreVA == 0x1016) {
        Store.StoreVA = 0x1140;
        Store.EndVA = 0x1147;
      }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
    EXPECT_EQ(A.CleanupFrameEffectsComplete, BothInitialized);
  }
}

namespace {
LowFunc makeCxxTypedObject(bool Reference) {
  auto F = makeCxxObjectCall();
  auto &Catch = F.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0];
  Catch.TypeDescriptorVA = 0x3000;
  Catch.CatchObjectOffset = -24;
  Catch.Adjectives = Reference ? 8 : 0;
  auto &Handler = F.Blocks[5];
  const auto Original = Handler.Ops;
  Handler.Ops.clear();
  emitOp(Handler, 0x1800, NdOp::INT_ADD, NdVar::tmp(60, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  if (Reference) {
    emitOp(Handler, 0x1800, NdOp::LOAD, NdVar::tmp(61, 4), {NdVar::tmp(60, 4)});
    emitOp(Handler, 0x1800, NdOp::LOAD, NdVar::tmp(62, 4), {NdVar::tmp(61, 4)});
    emitOp(Handler, 0x1800, NdOp::STORE, {},
           {NdVar::tmp(61, 4), NdVar::tmp(62, 4)});
  } else
    emitOp(Handler, 0x1800, NdOp::LOAD, NdVar::tmp(62, 4), {NdVar::tmp(60, 4)});
  for (auto Op : Original) {
    Op.Seq = Handler.Ops.size();
    Handler.Ops.push_back(Op);
  }
  return F;
}

RegistrationCalleeFrameContract scalarThrowContract() {
  RegistrationCalleeFrameContract C;
  C.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
  C.Target = 0x2100;
  C.DoesNotReturn = true;
  C.ThrownTypeVA = 0x3000;
  C.ThrownObjectSize = 4;
  return C;
}
} // namespace

TEST(RegistrationState, KeepsRuntimeExceptionObjectsSeparateFromParentFrames) {
  for (bool Reference : {false, true}) {
    SCOPED_TRACE(Reference);
    auto F = makeCxxTypedObject(Reference);
    const std::vector Calls{scalarThrowContract()};
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls);
    EXPECT_TRUE(A.Complete);
    EXPECT_TRUE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.ImageReadsComplete);
    EXPECT_TRUE(A.CxxCatchObjectsComplete);
    EXPECT_TRUE(A.RuntimeObjectAccessesComplete);
    ASSERT_EQ(A.CxxCatchObjects.size(), 1u);
    EXPECT_EQ(A.CxxCatchObjects[0],
              (RegistrationCxxCatchObject{0, 0, 0x3000, 4, -24, Reference}));
    ASSERT_EQ(A.RuntimeObjectAccesses.size(), Reference ? 2u : 0u);
    for (const auto &Access : A.RuntimeObjectAccesses) {
      EXPECT_EQ(Access.Address, 0x1800u);
      EXPECT_EQ(Access.Offset, 0);
      EXPECT_EQ(Access.Width, 4);
      EXPECT_EQ(Access.TryIndex, 0u);
      EXPECT_EQ(Access.CatchIndex, 0u);
    }
  }
}

TEST(RegistrationState, RejectsUnprovedRuntimeObjectBoundsAndPointerUses) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxTypedObject(true);
    std::vector Calls{scalarThrowContract()};
    auto &Handler = F.Blocks[5];
    auto &Catch = F.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0];
    switch (Mutation) {
    case 0:
      Calls[0].ThrownTypeVA = 0x3004;
      break;
    case 1:
      Calls[0].ThrownObjectSize = 2;
      break;
    case 2:
      Catch.CatchObjectOffset = -16;
      break;
    case 3:
      Catch.CatchObjectOffset = -32;
      break;
    case 4:
      Catch.Adjectives = 1;
      break;
    case 5:
      Handler.Ops[1].Output.Size = 2;
      break;
    case 6:
      Handler.Ops[2].Inputs[0] = NdVar::cst(0x4000, 4);
      Handler.Ops[3].Inputs[0] = NdVar::cst(0x4000, 4);
      // The original runtime pointer cannot be published to the image.
      Handler.Ops[3].Inputs[1] = NdVar::tmp(61, 4);
      break;
    case 7:
      Handler.Ops[3].Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      break;
    case 8:
      Handler.Ops[3].Opcode = NdOp::ATOMIC_XCHG;
      break;
    case 9:
      Handler.Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 10:
      // A stale pointer cannot be dereferenced after catchret closes its life.
      emitOp(F.Blocks[6], 0x1900, NdOp::INT_ADD, NdVar::tmp(70, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
      emitOp(F.Blocks[6], 0x1900, NdOp::LOAD, NdVar::tmp(71, 4),
             {NdVar::tmp(70, 4)});
      emitOp(F.Blocks[6], 0x1900, NdOp::LOAD, NdVar::tmp(72, 4),
             {NdVar::tmp(71, 4)});
      break;
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls);
    EXPECT_FALSE(A.RuntimeObjectAccessesComplete);
    EXPECT_TRUE(A.RuntimeObjectAccesses.empty());
  }
}

TEST(RegistrationState, RejectsUnprovenObjectAndCallProjections) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    auto F = makeCxxObjectCall();
    std::vector Contracts{objectLeafContract()};
    auto &Body = F.Blocks[1];
    auto ObjectStore =
        std::find_if(Body.Ops.begin(), Body.Ops.end(), [](const auto &Op) {
          return Op.Opcode == NdOp::STORE && Op.Inputs[0].isReg() &&
                 Op.Inputs[0].Offset == x86reg::RCX;
        });
    auto ObjectAddress =
        std::find_if(Body.Ops.begin(), Body.Ops.end(), [](const auto &Op) {
          return Op.Opcode == NdOp::INT_ADD && Op.Output.isReg() &&
                 Op.Output.Offset == x86reg::RCX;
        });
    ASSERT_NE(ObjectStore, Body.Ops.end());
    ASSERT_NE(ObjectAddress, Body.Ops.end());
    switch (Mutation) {
    case 0:
      Body.Ops.erase(ObjectStore);
      break;
    case 1:
      ObjectStore->Inputs[1] = NdVar::cst(7, 2);
      break;
    case 2:
      ObjectStore->Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      break;
    case 3:
      ObjectAddress->Inputs[1] = NdVar::cst(uint32_t(-12), 4);
      break;
    case 4:
      ObjectAddress->Inputs[1] = NdVar::cst(uint32_t(-32), 4);
      break;
    case 5:
      ObjectAddress->Opcode = NdOp::INT_XOR;
      ObjectAddress->Inputs[1] = ObjectAddress->Inputs[0];
      break;
    case 6:
      F.Blocks[7].InstructionBoundaries[0].Control =
          LowInstructionControl::None;
      break;
    case 7:
      Contracts[0].StackPopBytes = 4;
      break;
    case 8:
      F.Blocks[7].Ops[0].Inputs[0] = NdVar::cst(0x2200, 4);
      break;
    case 9:
      F.Blocks[7].Ops[0].Opcode = NdOp::INDIR_CALL;
      break;
    case 10:
      Contracts.push_back(Contracts.front());
      break;
    case 11:
      Contracts[0].ECXReads = {{0, 4}, {2, 6}};
      break;
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_FALSE(A.CallFrameEffectsComplete) << Mutation;
    EXPECT_TRUE(A.CallFrameEffects.empty()) << Mutation;
  }
}

TEST(RegistrationState, ObjectInitializationMustHoldOnEveryPredecessor) {
  for (bool BothInitialized : {false, true}) {
    auto F = makeCxxObjectCall();
    const std::vector Contracts{objectLeafContract()};
    auto &Body = F.Blocks[1];
    auto Store =
        std::find_if(Body.Ops.begin(), Body.Ops.end(), [](const auto &Op) {
          return Op.Opcode == NdOp::STORE && Op.Inputs[0].isReg() &&
                 Op.Inputs[0].Offset == x86reg::RCX;
        });
    ASSERT_NE(Store, Body.Ops.end());
    Body.Ops.erase(Store);
    Body.Succs = {8, 9};
    for (int Id : {8, 9}) {
      LowBlock B;
      B.Id = Id;
      B.StartAddr = 0x1120 + (Id - 8) * 8;
      B.EndAddr = B.StartAddr + 7;
      B.InstructionBoundaries = {{B.StartAddr, 7}};
      B.Succs = {7};
      if (Id == 8 || BothInitialized)
        emitOp(B, B.StartAddr, NdOp::STORE, {},
               {NdVar::reg(x86reg::RCX, 4), NdVar::cst(7, 4)});
      F.Blocks.push_back(std::move(B));
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(A.CallFrameEffectsComplete, BothInitialized);
    EXPECT_EQ(A.CallFrameEffects.size(), BothInitialized ? 1u : 0u);
  }
}

TEST(RegistrationState, PrivateThrowStopsOrdinaryFlowAndKeepsCatchResumption) {
  for (bool TrailingState : {false, true}) {
    auto F = makeCxxObjectCall();
    if (TrailingState) {
      auto &Call = F.Blocks[7];
      Call.EndAddr = 0x110d;
      Call.InstructionBoundaries.push_back({0x1105, 1});
      Call.InstructionBoundaries.push_back({0x1106, 7});
      F.ExceptionMetadata->Registration->TryLevelStores.push_back(
          {0x1106, 0x110d, -1});
      LowBlock DeadStore;
      DeadStore.StartAddr = 0x1106;
      addSlotStore(DeadStore, -1);
      Call.Ops.insert(Call.Ops.end(), DeadStore.Ops.begin(),
                      DeadStore.Ops.end());
    }
    RegistrationCalleeFrameContract Throw;
    Throw.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
    Throw.Target = 0x2100;
    Throw.DoesNotReturn = true;
    Throw.ThrownTypeVA = 0x3000;
    Throw.ThrownObjectSize = 4;
    const std::vector Contracts{Throw};
    const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_TRUE(A.Complete);
    EXPECT_TRUE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.RegistrationLifetimeComplete);
    EXPECT_TRUE(A.CxxContinuationsComplete);
    ASSERT_EQ(A.CallFrameEffects.size(), 1u);
    EXPECT_TRUE(A.CallFrameEffects[0].DoesNotReturn);
    EXPECT_EQ(A.CallFrameEffects[0].EndAddress, 0x1105u);
    EXPECT_TRUE(A.Blocks[2].Levels.empty());
    EXPECT_TRUE(A.Blocks[0].Reached);
    EXPECT_FALSE(A.Blocks[2].Reached);
    EXPECT_TRUE(A.Blocks[5].Reached);
    EXPECT_TRUE(A.Blocks[5].CallbackOnly);
    EXPECT_TRUE(A.Blocks[6].Reached);
    ASSERT_EQ(A.CxxContinuations.size(), 1u);
    EXPECT_EQ(A.CxxContinuations[0].TargetVA, 0x1900u);
  }
}

TEST(RegistrationState, FreedOrOpaqueFrameBytesCannotAuthorizeObjectReads) {
  for (bool Opaque : {false, true}) {
    auto F = makeCxxObjectCall();
    const std::vector Contracts{objectLeafContract()};
    LowBlock Effects;
    if (Opaque)
      emitOp(Effects, 0x1013, NdOp::INTRINSIC, {},
             {NdVar::cst(uint64_t(Intrinsic::Syscall), 2)});
    else {
      emitOp(Effects, 0x1013, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(12, 4)});
      emitOp(Effects, 0x1013, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(12, 4)});
    }
    auto &Body = F.Blocks[1];
    auto BeforeState =
        std::find_if(Body.Ops.begin(), Body.Ops.end(),
                     [](const auto &Op) { return Op.Addr == 0x1016; });
    Body.Ops.insert(BeforeState, Effects.Ops.begin(), Effects.Ops.end());
    const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_FALSE(A.CallFrameEffectsComplete) << Opaque;
    EXPECT_TRUE(A.CallFrameEffects.empty()) << Opaque;
  }
}

TEST(RegistrationState, APartialCalleeWriteCannotLaunderTheObjectPointer) {
  auto F = makeCxxObjectCall();
  auto &Body = F.Blocks[1];
  auto Store =
      std::find_if(Body.Ops.begin(), Body.Ops.end(), [](const auto &Op) {
        return Op.Opcode == NdOp::STORE && Op.Inputs[0].isReg() &&
               Op.Inputs[0].Offset == x86reg::RCX;
      });
  ASSERT_NE(Store, Body.Ops.end());
  Store->Inputs[1] = NdVar::reg(x86reg::RBP, 4);
  auto Writer = objectLeafContract();
  Writer.ECXReads.clear();
  Writer.ECXWrites = {{0, 1}};
  auto Reader = objectLeafContract();
  Reader.Target = 0x2200;
  const std::vector Contracts{Writer, Reader};
  F.Blocks[7].Succs = {8};
  LowBlock Reload;
  Reload.Id = 8;
  Reload.StartAddr = 0x1110;
  Reload.EndAddr = 0x1113;
  Reload.InstructionBoundaries = {{0x1110, 3}};
  Reload.Succs = {9};
  emitOp(Reload, 0x1110, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  LowBlock Read = F.Blocks[7];
  Read.Id = 9;
  Read.StartAddr = 0x1120;
  Read.EndAddr = 0x1125;
  Read.Succs = {2};
  Read.InstructionBoundaries[0].Address = 0x1120;
  Read.Ops[0].Addr = 0x1120;
  Read.Ops[0].Inputs[0] = NdVar::cst(0x2200, 4);
  F.Blocks.push_back(std::move(Reload));
  F.Blocks.push_back(std::move(Read));
  const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
  EXPECT_FALSE(A.CallFrameEffectsComplete);
  ASSERT_EQ(A.CallFrameEffects.size(), 1u);
  EXPECT_EQ(A.CallFrameEffects[0].Target, 0x2100u);
  EXPECT_EQ(A.callFrameEffect(0x1120, 0), nullptr);
}

TEST(RegistrationState, ADynamicWriteCannotKeepThePreviousState) {
  LowFunc F = makeBranchingFrame();
  addSlotStore(F.Blocks[3], 1);
  F.Blocks[3].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, LiftedStoreMustAgreeWithTheScanner) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, MissingSlotDoesNotInventScopeRanges) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelOffset.reset();
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostics.empty());
}

TEST(RegistrationState, MissingSuccessorFailsTheWholeProof) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[1].Succs.push_back(99);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, PreservesFrameAliasesAcrossInstructionsAndCFGEdges) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.back().Unknown);
}

TEST(RegistrationState, PreservesPossibleAliasesWhenOnlyOneBranchDefinesThem) {
  LowFunc F = makeBranchingFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, PreservesAliasesSpilledIntoKnownFrameCells) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, PartiallyOverwrittenSpillsKeepPossibleFrameAliases) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::cst(0, 1)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, ReassignedEBPDoesNotKeepTheOriginalSlotIdentity) {
  LowFunc F = makeBranchingFrame();
  LowOp Change;
  Change.Addr = 0x1010;
  Change.Opcode = NdOp::COPY;
  Change.Output = NdVar::reg(x86reg::RBP, 4);
  Change.addInput(NdVar::cst(0x800000, 4));
  F.Blocks[1].Ops.insert(F.Blocks[1].Ops.begin(), Change);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

LowFunc makeUnlinkedFrame() {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::cst(0, 8), NdVar::reg(x86reg::RCX, 4)},
         NdMemoryAddressSpace::X86FS);
  F.Blocks[3].Succs = {4};
  LowBlock Exit;
  Exit.Id = 4;
  Exit.StartAddr = 0x1040;
  Exit.EndAddr = 0x1041;
  F.Blocks.push_back(Exit);
  return F;
}

TEST(RegistrationState, UnlinkEndsTheLiveRegistration) {
  LowFunc F = makeUnlinkedFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  ASSERT_TRUE(Result.ChainOperationsComplete);
  ASSERT_EQ(Result.ChainAccesses.size(), 3u);
  EXPECT_EQ(Result.ChainAccesses[0].AccessKind,
            RegistrationChainAccess::Kind::ReadPreviousHead);
  EXPECT_EQ(Result.ChainAccesses[1].AccessKind,
            RegistrationChainAccess::Kind::Install);
  EXPECT_EQ(Result.ChainAccesses[2].AccessKind,
            RegistrationChainAccess::Kind::Remove);
  EXPECT_EQ(Result.ChainAccesses[2].Address, 0x1030u);
  EXPECT_EQ(Result.ChainAccesses[2].EndAddress, 0x1037u);
  EXPECT_EQ(Result.ChainAccesses[2].OpSeq, F.Blocks[3].Ops.back().Seq);
  EXPECT_TRUE(Result.Blocks.back().Levels.empty());
  EXPECT_FALSE(Result.Blocks.back().CanDispatch);
}

TEST(RegistrationState, MissingLifetimeCannotAuthorizeChainReplacement) {
  auto Result = analyzeRegistrationStates(makeBranchingFrame());
  ASSERT_TRUE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, ChainOwnershipRequiresUniqueOperationOccurrences) {
  LowFunc F = makeUnlinkedFrame();
  F.Blocks[0].Ops.back().Seq = 3; // Same address/seq as the previous-head load.
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, ChainOwnershipRequiresUniqueDecodedBoundaries) {
  LowFunc F = makeUnlinkedFrame();
  F.Blocks[0].InstructionBoundaries.push_back({0x1000, 6});
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, AnUnknownChainHeadWriteCannotKeepTheOldScope) {
  LowFunc F = makeBranchingFrame();
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::cst(0, 8), NdVar::reg(x86reg::RCX, 4)},
         NdMemoryAddressSpace::X86FS);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
}

TEST(RegistrationState, FinallyCanDispatchToItsEnclosingScope) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes.push_back({0, 0, 0x1040, true});
  Chain.TryLevelStores.front().Level = 1;
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
  LowBlock Finally;
  Finally.Id = 4;
  Finally.StartAddr = 0x1040;
  Finally.EndAddr = 0x1041;
  F.Blocks.push_back(Finally);
  LowBlock Filter;
  Filter.Id = 5;
  Filter.StartAddr = 0x1800;
  Filter.EndAddr = 0x1801;
  F.Blocks.push_back(Filter);
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[4].CallbackOnly);
  EXPECT_TRUE(Result.Blocks[4].CanDispatch);
  EXPECT_EQ(Result.Blocks[4].Levels, (std::vector<int32_t>{0}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
  EXPECT_FALSE(Result.Blocks[5].CanDispatch);
}

TEST(RegistrationState, AtomicFrameWritesInvalidateEvenWhenTheResultIsDead) {
  for (NdOp Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, Opcode, NdVar::tmp(24, 4),
           {NdVar::tmp(0, 4), NdVar::cst(0, 4), NdVar::cst(1, 4)});
    auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete);
    EXPECT_FALSE(Result.ChainOperationsComplete);
    EXPECT_TRUE(Result.FrameValues.empty());
  }
}

TEST(RegistrationState, AtomicFSWritesCannotBypassChainOwnership) {
  for (NdOp Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, Opcode, NdVar::tmp(24, 4),
           {NdVar::cst(0, 4), NdVar::cst(0, 4), NdVar::cst(1, 4)},
           NdMemoryAddressSpace::X86FS);
    auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete);
    EXPECT_FALSE(Result.ChainOperationsComplete);
    EXPECT_TRUE(Result.ChainAccesses.empty());
  }
}

TEST(RegistrationState, PublishesFramePointerProvenanceAfterSpillAndReload) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(24, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(24, 4)});
  const int ReloadSeq = F.Blocks[3].Ops.back().Seq;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  auto Reload =
      std::find_if(Result.FrameValues.begin(), Result.FrameValues.end(),
                   [&](const RegistrationFrameValue &Value) {
                     return Value.Address == 0x1030 && Value.OpSeq == ReloadSeq;
                   });
  ASSERT_NE(Reload, Result.FrameValues.end());
  EXPECT_EQ(Reload->EstablishedFrameOffset, -4);
}

TEST(RegistrationState, NarrowSpillsCannotLaunderFramePointerProvenance) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::reg(x86reg::RBP, 2)});
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(16, 4),
         {NdVar::tmp(8, 4), NdVar::cst(2, 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_RIGHT, NdVar::tmp(24, 2),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(16, 1)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(16, 4), NdVar::tmp(24, 2)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(24, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(24, 4)});
  const int ReloadSeq = F.Blocks[3].Ops.back().Seq;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  auto Reload =
      std::find_if(Result.FrameValues.begin(), Result.FrameValues.end(),
                   [&](const RegistrationFrameValue &Value) {
                     return Value.Address == 0x1030 && Value.OpSeq == ReloadSeq;
                   });
  ASSERT_NE(Reload, Result.FrameValues.end());
  EXPECT_FALSE(Reload->EstablishedFrameOffset);
}

TEST(RegistrationState, AtomicDesiredValueCannotExportTheRegistrationFrame) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(
      F.Blocks[1], 0x1010, NdOp::ATOMIC_CMPXCHG, NdVar::tmp(24, 4),
      {NdVar::cst(0x800000, 4), NdVar::cst(0, 4), NdVar::reg(x86reg::RBP, 4)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.FrameValues.empty());
}

TEST(RegistrationState,
     ImageReadClosureIncludesAtomicsAndRejectsUnknownMemory) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    LowFunc F = makeUnlinkedFrame();
    if (Kind == 1)
      emitOp(F.Blocks[1], 0x1010, NdOp::ATOMIC_ADD, NdVar::tmp(24, 4),
             {NdVar::cst(0x800002, 4), NdVar::cst(1, 4)});
    else
      emitOp(
          F.Blocks[1], 0x1010, NdOp::LOAD, NdVar::tmp(24, 4),
          {Kind == 2 ? NdVar::reg(x86reg::RAX, 4) : NdVar::cst(0x800002, 4)});
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    EXPECT_EQ(Result.ImageReadsComplete, Kind != 2);
    if (Kind != 2) {
      ASSERT_EQ(Result.ImageReads.size(), 1u);
      EXPECT_EQ(Result.ImageReads.front().Begin, 0x800002u);
      EXPECT_EQ(Result.ImageReads.front().End, 0x800006u);
    }
  }
}

TEST(RegistrationState, CallsInvalidatePreviouslyClearedVolatileRegisterFacts) {
  for (uint64_t Register : {x86reg::XMM0, x86reg::ZF}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(Register, 1),
           {NdVar::cst(0, 1)});
    emitOp(F.Blocks[1], 0x1010, NdOp::CALL, {}, {NdVar::cst(0x900000, 4)});
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::tmp(24, 1),
           {NdVar::reg(Register, 1)});
    const int Seq = F.Blocks[1].Ops.back().Seq;
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    auto Value = llvm::find_if(Result.FrameValues, [&](const auto &V) {
      return V.Address == 0x1010 && V.OpSeq == Seq;
    });
    ASSERT_NE(Value, Result.FrameValues.end());
    EXPECT_FALSE(Value->EstablishedFrameOffset);
  }
}

TEST(RegistrationState, OpaqueEntryRegistersKeepTheirPossibleFrameIdentity) {
  for (uint64_t Register :
       {x86reg::RAX, x86reg::RCX, x86reg::RBX, x86reg::ZF}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::tmp(24, 1),
           {NdVar::reg(Register, 1)});
    const int Seq = F.Blocks[1].Ops.back().Seq;
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    auto Value = llvm::find_if(Result.FrameValues, [&](const auto &V) {
      return V.Address == 0x1010 && V.OpSeq == Seq;
    });
    ASSERT_NE(Value, Result.FrameValues.end());
    EXPECT_FALSE(Value->EstablishedFrameOffset);
  }
}

} // namespace

TEST(RegistrationState, EmptyLevelsDoNotConflateUninstalledAndUnreachedFrames) {
  auto F = makeBranchingFrame();
  LowBlock Dead;
  Dead.Id = 9;
  Dead.StartAddr = 0x2000;
  Dead.EndAddr = 0x2001;
  Dead.InstructionBoundaries = {{Dead.StartAddr, 1}};
  emitOp(Dead, Dead.StartAddr, NdOp::RETURN, {}, {NdVar::cst(0, 4)});
  F.Blocks.push_back(Dead);
  const auto A = analyzeRegistrationStates(F);
  ASSERT_TRUE(A.Complete);
  ASSERT_EQ(A.Blocks.size(), F.Blocks.size());
  EXPECT_TRUE(A.Blocks[0].Reached);
  EXPECT_TRUE(A.Blocks[0].Levels.empty());
  EXPECT_FALSE(A.Blocks.back().Reached);
  EXPECT_TRUE(A.Blocks.back().Levels.empty());
  EXPECT_FALSE(A.Blocks.back().Unknown);
}

//===- RegistrationStateTests.cpp - Registration-state CFG proofs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationFrame.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <tuple>

namespace {

using namespace neverd;
using namespace neverd::registration_test;

TEST(RegistrationState, CxxDispatchBindingsCannotOutliveAnUnclosedOrigin) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = makeCxxCatchContinuation();
    auto &Stores = Function.ExceptionMetadata->Registration->TryLevelStores;
    std::erase_if(Stores,
                  [](const auto &Store) { return Store.StoreVA == 0x1020; });
    for (unsigned Index : {2u, 5u}) {
      auto &Block = Function.Blocks[Index];
      Block.Ops.clear();
      Block.Succs.clear();
      Block.EndAddr = Block.StartAddr + 5;
      Block.InstructionBoundaries = {{Block.StartAddr, 5}};
      auto &Call = Block.InstructionBoundaries.front();
      Call.Control = LowInstructionControl::Call;
      Call.ControlFlags =
          LowInstructionControlFlag::Call | LowInstructionControlFlag::NoReturn;
      emitOp(Block, Block.StartAddr, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
             {NdVar::cst(0x2100, 4)});
    }
    std::vector<RegistrationCalleeFrameContract> Callees(1);
    auto &Throw = Callees.front();
    Throw.Target = 0x2100;
    Throw.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
    Throw.DoesNotReturn = true;
    Throw.ThrownTypeVA = 0x4000;
    Throw.ThrownObjectSize = 4;
    Throw.RequiredImports = {0x3000};
    registration_state::CxxCallerProof Proof;
    Proof.ImportSlots.insert(0x3000);
    if (Mutation != 1)
      Proof.ProveDispatchCall = [&](const LowOp &,
                                    const RegistrationCalleeFrameContract &,
                                    const RegistrationCallFrameEffect &,
                                    size_t &) { return Mutation != 2; };
    Proof.ProveDispatchMemory = [](const LowOp &Op, const auto &, size_t &) {
      return Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
    };
    if (Mutation >= 3 && Mutation <= 6) {
      LowBlock Extra;
      if (Mutation == 3 || Mutation == 6)
        emitOp(Extra, 0x1013, NdOp::LOAD, NdVar::tmp(91, 4),
               {NdVar::cst(Mutation == 3 ? 0 : 0x4000, 4)});
      if (Mutation == 4)
        emitOp(Extra, 0x1013, NdOp::STORE, {},
               {NdVar::cst(0x3001, 4), NdVar::cst(0, 1)});
      if (Mutation == 5)
        emitOp(Extra, 0x1013, NdOp::INT_DIV, NdVar::tmp(91, 4),
               {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RCX, 4)});
      // Faults before the later state store must remain visible to the
      // following throw block; a closed later block cannot repair the proof.
      auto &Body = Function.Blocks[1];
      Body.Ops.insert(Body.Ops.begin() + 3, Extra.Ops.begin(), Extra.Ops.end());
    }
    if (Mutation == 8) {
      LowBlock Extra;
      emitOp(Extra, 0x1800, NdOp::LOAD, NdVar::tmp(91, 4), {NdVar::cst(0, 4)});
      auto &Handler = Function.Blocks[5];
      Handler.Ops.insert(Handler.Ops.begin(), Extra.Ops.front());
      Handler.Ops.back().Seq = 1;
    }
    if (Mutation == 7)
      std::reverse(Function.Blocks.begin(), Function.Blocks.end());
    registration_state::RegistrationStateSolver Solver(
        Function, 0, 0, &Callees, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &Proof);
    const auto States = Solver.run();
    const bool Closed = Mutation == 0 || Mutation == 7;
    if (Closed)
      ASSERT_TRUE(States.Complete)
          << (States.Diagnostics.empty() ? "" : States.Diagnostics.front());
    EXPECT_EQ(
        llvm::any_of(States.CallFrameEffects,
                     [](const auto &Call) { return Call.Address == 0x1800; }),
        Closed);
  }
}

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

TEST(RegistrationState, IndexedFrameAccessesKeepPartialAndUnknownAliases) {
  using registration_state::FrameState;
  using registration_state::FrameValue;
  for (unsigned Space = 0; Space != 3; ++Space) {
    for (int32_t Base : {INT32_MIN + 16, -4096, 0, INT32_MAX - 32}) {
      SCOPED_TRACE(Space);
      SCOPED_TRACE(Base);
      FrameState State;
      auto Store = [&](int32_t Offset, uint16_t Width, FrameValue Value) {
        if (Space == 0)
          State.store(Offset, Width, Value);
        else if (Space == 1)
          State.storeEntry(Offset, Width, Value);
        else
          State.storeCallback(Offset, Width, Value);
      };
      auto Load = [&](int32_t Offset, uint16_t Width) {
        return Space == 0   ? State.load(Offset, Width)
               : Space == 1 ? State.loadEntry(Offset, Width)
                            : State.loadCallback(Offset, Width);
      };
      Store(Base - 8, 4, FrameValue::constant(11));
      Store(Base, 4, FrameValue::frame(-128));
      Store(Base + 4, 4, FrameValue::frame(-256));
      Store(Base + 16, 4, FrameValue::constant(29));
      // One unaligned scalar store overlaps both pointers but replaces
      // neither whole cell. Untouched bytes still carry possible aliases.
      Store(Base + 2, 4, FrameValue::constant(7));
      EXPECT_TRUE(Load(Base, 1).MayBeFrame);
      EXPECT_TRUE(Load(Base + 6, 1).MayBeFrame);
      EXPECT_EQ(Load(Base + 2, 4), FrameValue::constant(7));
      EXPECT_FALSE(Load(Base, 4).Offset);
      EXPECT_TRUE(State.load(std::nullopt, 1).MayBeFrame);
      EXPECT_EQ(Load(Base - 8, 4), FrameValue::constant(11));
      EXPECT_EQ(Load(Base + 16, 4), FrameValue::constant(29));
      Store(Base - 1, 10, {});
      EXPECT_FALSE(Load(Base, 8).MayBeFrame);
      EXPECT_FALSE(State.load(std::nullopt, 1).MayBeFrame);
      EXPECT_EQ(Load(Base - 8, 4), FrameValue::constant(11));
      EXPECT_EQ(Load(Base + 16, 4), FrameValue::constant(29));
      Store(INT32_MIN, 4, FrameValue::frame(-64));
      EXPECT_TRUE(Load(INT32_MIN, 1).MayBeFrame);
      Store(INT32_MIN, 1, {});
      EXPECT_TRUE(Load(INT32_MIN + 3, 1).MayBeFrame);
      Store(INT32_MAX - 3, 4, FrameValue::frame(-96));
      EXPECT_TRUE(Load(INT32_MAX, 1).MayBeFrame);
      Store(INT32_MAX, 1, {});
      EXPECT_TRUE(Load(INT32_MAX - 3, 1).MayBeFrame);
    }
  }
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

TEST(RegistrationState, EH4CallbackScratchCannotAliasCookieStorage) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCookieFrame();
    auto &Block = F.Blocks.emplace_back();
    Block.Id = 4;
    Block.StartAddr = 0x1800;
    Block.EndAddr = 0x1811;
    emitOp(Block, 0x1800, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    NdVar Address = NdVar::reg(x86reg::RSP, 4);
    if (Mutation == 1 || Mutation == 2) {
      emitOp(Block, 0x1803, NdOp::INT_ADD, NdVar::tmp(40, 4),
             {NdVar::reg(x86reg::RBP, 4),
              NdVar::cst(uint32_t(Mutation == 1 ? -28 : -8), 4)});
      Address = NdVar::tmp(40, 4);
    } else if (Mutation == 3)
      Address = NdVar::cst(0x4000, 4);
    else if (Mutation == 4)
      Address = NdVar::reg(x86reg::RCX, 4);
    emitOp(Block, 0x1803, NdOp::STORE, {}, {Address, NdVar::cst(1, 4)});
    emitOp(Block, 0x180a, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    emitOp(Block, 0x180d, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Block, 0x1810, NdOp::RETURN, {}, {});
    Block.InstructionBoundaries = {
        {0x1800, 3}, {0x1803, 7}, {0x180a, 3}, {0x180d, 3}, {0x1810, 1}};
    Block.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    const auto Result = analyzeRegistrationStates(F, 0x4000);
    EXPECT_EQ(Result.SecurityCookiesComplete, Mutation == 0);
    if (!Mutation) {
      EXPECT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.CallbackStatesComplete);
    }
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
  // This analysis fixture has decoded extents but no operation indices.
  // Missing native provenance must not discard its checked state semantics.
  EXPECT_FALSE(Result.SEHStateStoresComplete);
  EXPECT_TRUE(Result.SEHStateStores.empty());
  ASSERT_EQ(Result.Blocks.size(), 4u);
  EXPECT_EQ(Result.Blocks[3].levels(), (std::vector<int32_t>{-1, 0}));
  EXPECT_FALSE(registrationRangesWhere(
      Result, [](int32_t Level) { return Level == 0; }));
}

TEST(RegistrationState, ReplaysLoopBackedges) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[3].Succs = {1};
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].levels().empty());
  EXPECT_EQ(Result.Blocks[1].levels(), (std::vector<int32_t>{-1, 0}));
}

TEST(RegistrationState, DoesNotSeedBeforeTheRegistrationIsInstalled) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].levels().empty());
  EXPECT_EQ(Result.Blocks[1].levels(), (std::vector<int32_t>{-1}));
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
  EXPECT_TRUE(Result.Blocks[2].levels().empty());
  EXPECT_EQ(Result.Blocks[3].levels(), (std::vector<int32_t>{0}));
}

TEST(RegistrationState, KeepsTheIncomingLevelUntilTheStoreRetires) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_EQ(Result.Blocks[1].levels(), (std::vector<int32_t>{-1}));
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
    EXPECT_EQ(Result.Blocks.back().levels(), Expected);
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

LowFunc makeSEHObjectCall(bool EH4, bool GS, int32_t Offset, uint16_t Width,
                          bool Initialize) {
  auto F = EH4 ? makeCookieFrame(GS) : makeBranchingFrame();
  const auto BP = NdVar::reg(x86reg::RBP, 4);
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto Object = NdVar::reg(x86reg::RCX, 4);
  if (!EH4) {
    auto &Exit = F.Blocks[3];
    emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(30, 4),
           {BP, NdVar::cst(uint32_t(-16), 4)});
    emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::tmp(31, 4),
           {NdVar::tmp(30, 4)});
    emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
           {NdVar::cst(0, 4), NdVar::tmp(31, 4)}, NdMemoryAddressSpace::X86FS);
  }
  LowBlock Call;
  Call.Id = F.Blocks.size();
  Call.StartAddr = 0x1100;
  Call.Succs = {1, 2};
  va_t Address = Call.StartAddr;
  auto Emit = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs, uint8_t Size) {
    const size_t First = Call.Ops.size();
    emitOp(Call, Address, Opcode, Output, Inputs);
    Call.InstructionBoundaries.push_back({Address, Size, First, 1});
    Address += Size;
  };
  Emit(NdOp::INT_SUB, SP, {SP, NdVar::cst(48, 4)}, 3);
  Emit(NdOp::INT_ADD, Object, {BP, NdVar::cst(uint32_t(Offset), 4)}, 3);
  if (Initialize)
    Emit(NdOp::STORE, {}, {Object, NdVar::cst(7, Width)}, 3);
  Emit(NdOp::CALL, {}, {NdVar::cst(0x2100, 4)}, 5);
  Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Call.EndAddr = Address;
  F.Blocks[0].Succs = {Call.Id};
  F.Blocks.push_back(std::move(Call));
  return F;
}

TEST(RegistrationState, SEHObjectBorrowsExcludeSavedStackAndExceptionPointers) {
  for (bool EH4 : {false, true}) {
    for (bool Write : {false, true}) {
      for (const auto &[Offset, Width, Allowed] :
           {std::tuple{-32, uint16_t(4), true},
            std::tuple{-28, uint16_t(4), !EH4},
            std::tuple{-25, uint16_t(1), !EH4},
            std::tuple{-25, uint16_t(4), false},
            std::tuple{-24, uint16_t(4), false},
            std::tuple{-21, uint16_t(1), false},
            std::tuple{-20, uint16_t(4), false},
            std::tuple{-17, uint16_t(1), false}}) {
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Write);
        SCOPED_TRACE(Offset);
        SCOPED_TRACE(Width);
        const auto F = makeSEHObjectCall(EH4, false, Offset, Width, !Write);
        RegistrationCalleeFrameContract Leaf;
        Leaf.Target = 0x2100;
        (Write ? Leaf.ECXWrites : Leaf.ECXReads) = {{0, Width}};
        const std::vector Callees{Leaf};
        const auto A =
            analyzeRegistrationStates(F, EH4 ? 0x4000 : 0, 0, &Callees);
        EXPECT_EQ(A.CallFrameEffectsComplete, Allowed);
        EXPECT_EQ(A.CallFrameEffects.empty(), !Allowed);
        EXPECT_EQ(A.callFrameEffect(F.Blocks.back().Ops.back().Addr,
                                    F.Blocks.back().Ops.back().Seq) != nullptr,
                  Allowed);
      }
    }
  }
}

TEST(RegistrationState, SEHObjectWritesCannotBorrowWholeOrPartialCookieCells) {
  for (const auto &[Offset, Width, Allowed] :
       {std::tuple{-40, uint16_t(4), true}, std::tuple{-37, uint16_t(4), false},
        std::tuple{-36, uint16_t(4), false},
        std::tuple{-33, uint16_t(1), false}, std::tuple{-32, uint16_t(4), true},
        std::tuple{-29, uint16_t(4), false},
        std::tuple{-28, uint16_t(4), false},
        std::tuple{-25, uint16_t(1), false}}) {
    SCOPED_TRACE(Offset);
    SCOPED_TRACE(Width);
    auto F = makeSEHObjectCall(true, true, Offset, Width, false);
    RegistrationCalleeFrameContract Leaf;
    Leaf.Target = 0x2100;
    Leaf.ECXWrites = {{0, Width}};
    const std::vector Callees{Leaf};
    const auto A = analyzeRegistrationStates(F, 0x4000, 0, &Callees);
    EXPECT_EQ(A.CallFrameEffectsComplete, Allowed);
    EXPECT_EQ(A.CallFrameEffects.empty(), !Allowed);
  }
}

TEST(RegistrationState, MalformedSEHScopeGraphsPublishNoStateFacts) {
  for (bool EH4 : {false, true}) {
    const auto Base = EH4 ? makeCookieFrame() : makeBranchingFrame();
    ASSERT_TRUE(analyzeRegistrationStates(Base, EH4 ? 0x4000 : 0).Complete);
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Mutation);
      auto F = Base;
      auto &Chain = *F.ExceptionMetadata->Registration;
      const int32_t InvalidLevels[] = {EH4 ? -1 : -2, -3, 0, 1, 99};
      if (Mutation < 5)
        Chain.Scopes[0].EnclosingLevel = InvalidLevels[Mutation];
      else if (Mutation == 5)
        Chain.Scopes[0].HandlerVA = 0;
      else if (Mutation == 6)
        Chain.Scopes[0].IsFinally = !Chain.Scopes[0].IsFinally;
      else if (Mutation == 7)
        Chain.SeededTryLevel = EH4 ? -1 : -2;
      else if (Mutation == 8)
        Chain.SeededTryLevel = 1;
      else if (Mutation == 9)
        Chain.Scopes[0].HandlerVA = InvalidVA;
      else if (Mutation == 10)
        Chain.Scopes[0].HandlerVA = uint64_t(UINT32_MAX) + 1;
      else
        Chain.Scopes[0].FilterVA = uint64_t(UINT32_MAX) + 1;
      const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0);
      EXPECT_FALSE(A.Complete);
      EXPECT_FALSE(A.RegistrationLifetimeComplete);
      EXPECT_TRUE(A.Blocks.empty());
      EXPECT_TRUE(A.FrameValues.empty());
      EXPECT_TRUE(std::any_of(
          A.Diagnostics.begin(), A.Diagnostics.end(), [](const std::string &D) {
            return D.find("scope graph") != std::string::npos;
          }));
    }
  }
}

TEST(RegistrationState, InitialActiveSEHScopeRetainsItsFrameSentinel) {
  for (bool EH4 : {false, true}) {
    auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    const int32_t Outside = EH4 ? -2 : -1;
    Chain.SeededTryLevel = 0;
    for (auto &Op : F.Blocks[0].Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Size == 4 &&
          Op.Inputs[1].Offset == uint32_t(Outside))
        Op.Inputs[1] = NdVar::cst(0, 4);
    const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0);
    ASSERT_TRUE(A.Complete);
    ASSERT_EQ(A.Blocks.size(), F.Blocks.size());
    EXPECT_EQ(A.Blocks.back().levels(), (std::vector<int32_t>{Outside, 0}));
    if (EH4)
      EXPECT_TRUE(A.SecurityCookiesComplete);
  }
}

TEST(RegistrationState, EmptySEHScopeTableKeepsItsOutsideState) {
  for (bool EH4 : {false, true}) {
    auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    const int32_t Outside = EH4 ? -2 : -1;
    Chain.Scopes.clear();
    Chain.TryLevelStores[0].Level = Outside;
    F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(Outside), 4);
    const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0);
    ASSERT_TRUE(A.Complete);
    ASSERT_EQ(A.Blocks.size(), F.Blocks.size());
    EXPECT_EQ(A.Blocks.back().levels(), (std::vector<int32_t>{Outside}));
  }
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

TEST(RegistrationState,
     CachedImportCleanupRequiresItsCurrentLoadAndReachingValue) {
  for (bool EH4 : {false, true})
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Mutation);
      auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
      LowBlock Load;
      Load.Id = 4;
      Load.StartAddr = 0x1100;
      Load.EndAddr = 0x1106;
      Load.Succs = {5};
      Load.InstructionBoundaries = {{0x1100, 6, 0, 4}};
      emitOp(Load, 0x1100, NdOp::COPY, NdVar::tmp(0, 4),
             {NdVar::cst(0x2100, 4)});
      emitOp(Load, 0x1100, NdOp::INT_ZEXT, NdVar::tmp(8, 8),
             {NdVar::tmp(0, 4)});
      emitOp(Load, 0x1100, NdOp::LOAD, NdVar::tmp(16, 4), {NdVar::tmp(8, 8)});
      emitOp(Load, 0x1100, NdOp::COPY, NdVar::reg(x86reg::RSI, 4),
             {NdVar::tmp(16, 4)});
      LowBlock Call;
      Call.Id = 5;
      Call.StartAddr = 0x1110;
      Call.EndAddr = 0x1117;
      Call.Succs = {1, 2};
      Call.InstructionBoundaries = {
          {0x1110, 2, 0, 1}, {0x1112, 3, 1, 1}, {0x1115, 2, 2, 1}};
      Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
      emitOp(Call, 0x1110, NdOp::INDIR_CALL, {}, {NdVar::reg(x86reg::RSI, 4)});
      emitOp(Call, 0x1112, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
      emitOp(Call, 0x1115, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
      F.Blocks[0].Succs = {4};
      std::vector<RegistrationCalleeStackContract> Stacks{
          {0x2100, 0, true, {{0x1100, 2}}}};
      if (Mutation == 1)
        Stacks[0].Loads.clear();
      if (Mutation == 2)
        Stacks[0].Loads[0].second = 3;
      if (Mutation == 3)
        Stacks[0].Target += 4;
      if (Mutation == 4)
        Load.Ops[2].Output.Size = 2;
      if (Mutation == 5)
        Stacks[0].Loads.push_back(Stacks[0].Loads[0]);
      if (Mutation == 6)
        Load.Ops[0].Inputs[0].Offset += 4;
      if (Mutation == 7)
        Load.Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 8)
        Call.Ops[0].Inputs[0] = NdVar::reg(x86reg::RCX, 4);
      if (Mutation == 9)
        Load.Ops[3].Output.Size = 2;
      if (Mutation == 10)
        F.Blocks[0].Succs.push_back(5); // One reaching path never loaded ESI.
      if (Mutation == 11)
        Stacks[0].Indirect = false;
      if (Mutation == 12)
        Call.Ops[0].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 13)
        Call.Ops[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation >= 14) {
        LowBlock Intermediary;
        Intermediary.Id = 6;
        Intermediary.StartAddr = 0x1120;
        Intermediary.EndAddr = 0x1136;
        Intermediary.Succs = {5};
        Load.Succs = {6};
        const va_t CalledSlot = Mutation >= 17 ? 0x2200 : 0x2100;
        if (Mutation >= 17)
          Stacks.push_back({CalledSlot, 0, true, {}});
        if (Mutation == 15 || Mutation == 17 || Mutation == 19) {
          Intermediary.InstructionBoundaries.push_back({0x1120, 10, 0, 1});
          emitOp(Intermediary, 0x1120, NdOp::STORE, {},
                 {NdVar::cst(Mutation == 19 ? 0x2300 : CalledSlot, 4),
                  NdVar::cst(0, 4)});
        }
        if (Mutation == 16) {
          Intermediary.InstructionBoundaries.push_back({0x1120, 6, 0, 1});
          Intermediary.InstructionBoundaries.back().Control =
              LowInstructionControl::Call;
          emitOp(Intermediary, 0x1120, NdOp::INDIR_CALL, {},
                 {NdVar::cst(0x2100, 4)});
        }
        Intermediary.InstructionBoundaries.push_back(
            {0x1130, 6, Intermediary.Ops.size(), 1});
        Intermediary.InstructionBoundaries.back().Control =
            LowInstructionControl::Call;
        emitOp(Intermediary, 0x1130, NdOp::INDIR_CALL, {},
               {NdVar::cst(CalledSlot, 4)});
        F.Blocks.push_back(std::move(Intermediary));
      }
      F.Blocks.push_back(std::move(Load));
      F.Blocks.push_back(std::move(Call));
      const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0, 0, nullptr,
                                               nullptr, &Stacks);
      EXPECT_EQ(A.Complete, Mutation == 0 || Mutation == 14 || Mutation >= 18);
      EXPECT_FALSE(A.CallFrameEffectsComplete);
      EXPECT_TRUE(A.CallFrameEffects.empty());
      EXPECT_TRUE(A.CalleeContracts.empty());
    }
}

TEST(RegistrationState, ImportedStackCleanupRequiresItsCurrentBinding) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
        LowBlock Call;
        Call.Id = 4;
        Call.StartAddr = 0x1100;
        Call.EndAddr = 0x1136;
        Call.Succs = {1, 2};
        const va_t Target = Indirect ? 0x2200 : 0x2100;
        std::vector<RegistrationCalleeStackContract> Stacks{
            {Target,
             0,
             Indirect,
             {},
             Indirect ? std::vector<va_t>{} : std::vector<va_t>{0x2200}}};
        if (Mutation >= 1 && Mutation <= 4) {
          Call.InstructionBoundaries.push_back({0x1100, 10, 0, 1});
          const auto Address = Mutation == 4
                                   ? NdVar::reg(x86reg::RCX, 4)
                                   : NdVar::cst(Mutation == 2   ? 0x2300
                                                : Mutation == 3 ? 0x2203
                                                                : 0x2200,
                                                4);
          emitOp(Call, 0x1100, NdOp::STORE, {},
                 {Address, NdVar::cst(0, Mutation == 3 ? 1 : 4)});
        }
        if (Mutation == 5) {
          Call.InstructionBoundaries.push_back({0x1100, 5, 0, 1});
          Call.InstructionBoundaries.back().Control =
              LowInstructionControl::Call;
          emitOp(Call, 0x1100, NdOp::CALL, {}, {NdVar::cst(0x2300, 4)});
          Stacks.push_back({0x2300, 0, false});
        }
        if (Mutation == 6)
          Stacks[0].RequiredImports = {0x2200, 0x2200};
        Call.InstructionBoundaries.push_back(
            {0x1110, uint16_t(Indirect ? 6 : 5), Call.Ops.size(), 1});
        Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
        emitOp(Call, 0x1110, Indirect ? NdOp::INDIR_CALL : NdOp::CALL, {},
               {NdVar::cst(Target, 4)});
        if (Mutation == 7) {
          Call.InstructionBoundaries.push_back(
              {0x1120, 10, Call.Ops.size(), 1});
          emitOp(Call, 0x1120, NdOp::STORE, {},
                 {NdVar::cst(0x2200, 4), NdVar::cst(0, 4)});
        }
        Call.InstructionBoundaries.push_back({0x1130, 3, Call.Ops.size(), 1});
        emitOp(Call, 0x1130, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
               {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
        Call.InstructionBoundaries.push_back({0x1133, 3, Call.Ops.size(), 1});
        emitOp(Call, 0x1133, NdOp::STORE, {},
               {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
        F.Blocks[0].Succs = {4};
        F.Blocks.push_back(std::move(Call));
        const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0, 0,
                                                 nullptr, nullptr, &Stacks);
        EXPECT_EQ(A.Complete, Mutation == 0 || Mutation == 2 || Mutation == 7);
        EXPECT_FALSE(A.CallFrameEffectsComplete);
        EXPECT_TRUE(A.CallFrameEffects.empty());
      }
}

TEST(RegistrationState, ImportBindingAndRegisterEffectsRemainIndependent) {
  for (bool EH4 : {false, true})
    for (bool Veneer : {false, true})
      for (bool PreservesBindings : {false, true})
        for (bool CachedCall : {false, true})
          for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
            SCOPED_TRACE(::testing::Message()
                         << EH4 << ' ' << Veneer << ' ' << PreservesBindings
                         << ' ' << CachedCall << ' ' << Mutation);
            auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
            LowBlock Calls;
            Calls.Id = 4;
            Calls.StartAddr = 0x1100;
            Calls.EndAddr = 0x1156;
            Calls.Succs = {1, 2};
            const auto Boundary = [&](va_t Address, uint16_t Size,
                                      bool IsCall = false) {
              Calls.InstructionBoundaries.push_back(
                  {Address, Size, Calls.Ops.size(), 1});
              if (IsCall)
                Calls.InstructionBoundaries.back().Control =
                    LowInstructionControl::Call;
            };
            Boundary(0x1100, 6);
            Calls.InstructionBoundaries.back().OpCount = 4;
            emitOp(Calls, 0x1100, NdOp::COPY, NdVar::tmp(0, 4),
                   {NdVar::cst(0x2100, 4)});
            emitOp(Calls, 0x1100, NdOp::INT_ZEXT, NdVar::tmp(8, 8),
                   {NdVar::tmp(0, 4)});
            emitOp(Calls, 0x1100, NdOp::LOAD, NdVar::tmp(16, 4),
                   {NdVar::tmp(8, 8)});
            emitOp(Calls, 0x1100, NdOp::COPY, NdVar::reg(x86reg::RSI, 4),
                   {NdVar::tmp(16, 4)});
            std::vector<RegistrationCalleeStackContract> Stacks{
                {0x2100, 0, true, {{0x1100, 2}}}};
            Stacks[0].PreservesImportBindings = PreservesBindings;
            if (Veneer) {
              Stacks.push_back({0x2200, 0, false, {}, {0x2100}});
              Stacks.back().ImportVeneer = true;
              Stacks.back().PreservesImportBindings = PreservesBindings;
            }
            if (Mutation == 1) {
              Boundary(0x1110, 10);
              emitOp(Calls, 0x1110, NdOp::STORE, {},
                     {NdVar::cst(0x2100, 4), NdVar::cst(0, 4)});
            }
            if (Mutation == 2 || Mutation == 6 || Mutation == 7) {
              Boundary(0x1110, 5, true);
              emitOp(Calls, 0x1110, NdOp::CALL, {}, {NdVar::cst(0x2300, 4)});
              Stacks.push_back({0x2300, 0, false});
              Stacks.back().PreservesImportBindings = Mutation == 6;
            }
            if (Mutation == 3) {
              if (Veneer)
                Stacks.back().RequiredImports.clear();
              else
                Stacks[0].ImportVeneer = true;
            }
            if (Mutation == 5) {
              Stacks.push_back({0x2300, uint32_t(UINT16_MAX) + 1, false});
              Stacks.back().PreservesImportBindings = true;
            }
            Boundary(0x1120, Veneer ? 5 : 6, true);
            emitOp(Calls, 0x1120, Veneer ? NdOp::CALL : NdOp::INDIR_CALL, {},
                   {NdVar::cst(Veneer ? 0x2200 : 0x2100, 4)});
            if (Mutation == 4) {
              Boundary(0x1130, 10);
              emitOp(Calls, 0x1130, NdOp::STORE, {},
                     {NdVar::cst(0x2100, 4), NdVar::cst(0, 4)});
            }
            Boundary(0x1140, CachedCall ? 2 : 6, true);
            emitOp(Calls, 0x1140, NdOp::INDIR_CALL, {},
                   {CachedCall ? NdVar::reg(x86reg::RSI, 4)
                               : NdVar::cst(0x2100, 4)});
            Boundary(0x1150, 3);
            emitOp(Calls, 0x1150, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
                   {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
            Boundary(0x1153, 3);
            emitOp(Calls, 0x1153, NdOp::STORE, {},
                   {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
            F.Blocks[0].Succs = {4};
            F.Blocks.push_back(std::move(Calls));
            const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0, 0,
                                                     nullptr, nullptr, &Stacks);
            EXPECT_EQ(A.Complete,
                      (Mutation == 0 && (CachedCall || PreservesBindings)) ||
                          (Mutation == 4 && CachedCall) ||
                          (Mutation == 6 && !CachedCall && PreservesBindings));
            EXPECT_FALSE(A.CallFrameEffectsComplete);
            EXPECT_TRUE(A.CallFrameEffects.empty());
            EXPECT_TRUE(A.SEHBoundCalls.empty());
          }
}

TEST(RegistrationState, CxxCalleeEffectsRequireLiveTransitiveImports) {
  for (bool EH4 : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << EH4 << ' ' << Mutation);
      auto F = makeSEHObjectCall(EH4, false, -32, 4, false);
      auto &Calls = F.Blocks.back();
      RegistrationCalleeFrameContract First;
      First.Target = 0x2100;
      if (Mutation == 1)
        First.ImageWrites = {{0x2200, 0x2204}};
      if (Mutation == 2)
        First.ImageWrites = {{0x2203, 0x2204}};
      if (Mutation == 3)
        First.ImageWrites = {{0x21ff, 0x2201}};
      if (Mutation == 4)
        First.ImageWrites = {{0x2204, 0x2208}};
      RegistrationCalleeFrameContract Parent;
      Parent.Target = 0x2400;
      Parent.CalleeKind = RegistrationCalleeFrameContract::Kind::CxxParent;
      Parent.RequiredImports = {0x2200};
      Parent.HasIndependentScalarReturn = true;
      if (Mutation == 6)
        Parent.RequiredImports.clear();
      if (Mutation == 7)
        Parent.RequiredImports.push_back(0x2200);
      const auto AppendCall = [&](va_t Target) {
        const va_t At = Calls.EndAddr;
        Calls.InstructionBoundaries.push_back({At, 5, Calls.Ops.size(), 1});
        Calls.InstructionBoundaries.back().Control =
            LowInstructionControl::Call;
        emitOp(Calls, At, NdOp::CALL, {}, {NdVar::cst(Target, 4)});
        Calls.EndAddr += 5;
      };
      if (Mutation == 5)
        AppendCall(0x2500); // Unknown effects lose bindings.
      AppendCall(Parent.Target);
      AppendCall(Parent.Target); // A complete first invocation retains them.
      const std::vector Callees{First, Parent};
      const auto A =
          analyzeRegistrationStates(F, EH4 ? 0x4000 : 0, 0, &Callees);
      EXPECT_EQ(A.CallFrameEffectsComplete, Mutation == 0 || Mutation == 4);
      EXPECT_EQ(A.callFrameEffect(Calls.Ops.back().Addr,
                                  Calls.Ops.back().Seq) != nullptr,
                Mutation == 0 || Mutation == 4);
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
  C.Calls.emplace_back();
  C.Calls[0].ObjectFrameOffset = -24;
  C.Calls[0].Leaf = objectLeafContract();
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

TEST(RegistrationState, CleanupObjectsStayCorrelatedWithTheirLiveState) {
  for (bool Reverse : {false, true})
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Reverse << ' ' << Mutation);
      auto F = makeCxxObjectCleanup();
      auto &EH = *F.ExceptionMetadata;
      EH.Cxx->TryBlocks.clear();
      // The object-free branch joins the live-object branch immediately
      // before resetting its state. Only state zero owns a cleanup borrow.
      auto Skip = F.Blocks[1];
      Skip.Id = 8;
      Skip.StartAddr += 0x200;
      Skip.EndAddr += 0x200;
      Skip.Succs = {2};
      std::erase_if(Skip.Ops, [](const auto &Op) {
        return Op.Addr == 0x1013 &&
               (Op.Output == NdVar::reg(x86reg::RCX, 4) ||
                (Op.Opcode == NdOp::STORE &&
                 Op.Inputs[0] == NdVar::reg(x86reg::RCX, 4)));
      });
      const int32_t Level = Mutation == 1 ? 0 : -1;
      Skip.Ops.back().Inputs[1] = NdVar::cst(uint32_t(Level), 4);
      for (size_t I = 0; I != Skip.Ops.size(); ++I) {
        Skip.Ops[I].Addr += 0x200;
        Skip.Ops[I].Seq = I;
      }
      for (auto &Boundary : Skip.InstructionBoundaries)
        Boundary.Address += 0x200;
      EH.Registration->TryLevelStores.push_back({0x1216, 0x121d, Level});
      F.Blocks[0].Succs = {1, Skip.Id};
      F.Blocks.push_back(std::move(Skip));
      if (Mutation == 2)
        for (auto &Op : F.Blocks[1].Ops)
          if (Op.Opcode == NdOp::STORE &&
              Op.Inputs[0] == NdVar::reg(x86reg::RCX, 4))
            Op.Inputs[1] = NdVar::cst(7, 1);
      if (Reverse)
        std::reverse(F.Blocks.begin(), F.Blocks.end());
      const std::vector Calls{objectLeafContract()};
      const std::vector Cleanups{objectCleanupContract()};
      const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
      EXPECT_EQ(A.Complete, Mutation != 2);
      EXPECT_EQ(A.CleanupFrameEffectsComplete, Mutation == 0);
    }
}

TEST(RegistrationState, SharedUnwindTailsKeepEachSourceLevelProof) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxObjectCleanup();
    auto &EH = *F.ExceptionMetadata;
    EH.Cxx->TryBlocks.clear();
    EH.Cxx->MaxState = 3;
    EH.Cxx->UnwindMap.resize(3);
    EH.Cxx->UnwindMap[1] = {0, 0, CxxUnwindAction::ActionKind::None};
    EH.Cxx->UnwindMap[2] = {0, 0, CxxUnwindAction::ActionKind::None};
    F.Blocks[1].Succs = {8, 9};
    for (unsigned I = 0; I != 2; ++I) {
      LowBlock Branch;
      Branch.Id = 8 + I;
      Branch.StartAddr = 0x1200 + I * 0x20;
      Branch.EndAddr = Branch.StartAddr + 7;
      Branch.InstructionBoundaries.push_back({Branch.StartAddr, 7});
      Branch.Succs = {7};
      addSlotStore(Branch, I + 1);
      EH.Registration->TryLevelStores.push_back(
          {Branch.StartAddr, Branch.EndAddr, int32_t(I + 1)});
      F.Blocks.push_back(std::move(Branch));
    }
    const std::vector Calls{objectLeafContract()};
    std::vector Cleanups{objectCleanupContract()};
    if (Mutation == 1)
      Cleanups.clear();
    if (Mutation == 2)
      Cleanups[0].Calls[0].Leaf.ECXReads[0].End = 8;
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
    ASSERT_TRUE(A.Complete);
    EXPECT_EQ(A.CleanupFrameEffectsComplete, Mutation == 0);
    std::set<int32_t> Levels;
    for (const auto &Effect : A.CleanupFrameEffects)
      if (Effect.BlockId == 7) {
        EXPECT_EQ(Effect.ActionState, 0u);
        EXPECT_EQ(Effect.FrameReads,
                  (std::vector<RegistrationObjectExtent>{{-24, -20}}));
        Levels.insert(Effect.DispatchLevel);
      }
    EXPECT_EQ(Levels,
              Mutation == 0 ? (std::set<int32_t>{1, 2}) : std::set<int32_t>{});
  }
}

TEST(RegistrationState, UnknownUnwindStateVisitsEachSharedCallbackTail) {
  for (bool CheckCleanups : {false, true})
    for (bool ReverseBlocks : {false, true})
      for (unsigned BodyBlocks : {0u, 256u, 2048u}) {
        SCOPED_TRACE(::testing::Message()
                     << CheckCleanups << "/" << ReverseBlocks << "/"
                     << BodyBlocks);
        auto F = makeNarrowCxxStateFrame(4, 0, 0, 0);
        auto &Cxx = *F.ExceptionMetadata->Cxx;
        Cxx.MaxState = limits::kMaxRegistrationEHRecords;
        Cxx.UnwindMap.clear();
        for (uint32_t I = 0; I != Cxx.MaxState; ++I)
          Cxx.UnwindMap.push_back(
              {int32_t(I) - 1, 0, CxxUnwindAction::ActionKind::None});
        // Distinct callback roots occur in suffixes shared by many levels.
        Cxx.UnwindMap[0].ActionVA = 0x1800;
        Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::Direct;
        Cxx.UnwindMap[2048].ActionVA = 0x1810;
        Cxx.UnwindMap[2048].Kind = CxxUnwindAction::ActionKind::Direct;
        // This unproved state value must remain unknown. Its conservative
        // callback roots are still required, without enumerating every shared
        // unwind suffix once per possible originating level.
        F.Blocks[3].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
        for (unsigned I = 0; I != 2; ++I) {
          LowBlock Callback;
          Callback.Id = 5 + I;
          Callback.StartAddr = 0x1800 + I * 0x10;
          Callback.EndAddr = Callback.StartAddr + 1;
          Callback.InstructionBoundaries.push_back({Callback.StartAddr, 1});
          F.Blocks.push_back(std::move(Callback));
        }
        // The unknown state can persist through a large ordinary body. Every
        // point contributes its own frame snapshot to the same callback roots;
        // the immutable unwind graph must not consume the budget again at each
        // point. Publishing the same unknown domain must also stay bounded
        // without dropping any possible level from an individual block.
        if (BodyBlocks) {
          F.Blocks[4].Succs = {7};
          for (unsigned I = 0; I != BodyBlocks; ++I) {
            LowBlock Body;
            Body.Id = 7 + I;
            Body.StartAddr = 0x3000 + I * 4;
            Body.EndAddr = Body.StartAddr + 1;
            Body.InstructionBoundaries.push_back({Body.StartAddr, 1});
            emitOp(Body, Body.StartAddr, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                   {NdVar::cst(I, 4)});
            if (I + 1 != BodyBlocks)
              Body.Succs = {Body.Id + 1};
            F.Blocks.push_back(std::move(Body));
          }
          F.ExceptionMetadata->CodeRange.End = F.Blocks.back().EndAddr;
        }
        if (ReverseBlocks)
          std::reverse(F.Blocks.begin(), F.Blocks.end());
        const std::vector<RegistrationCleanupFrameContract> Cleanups;
        const auto A = analyzeRegistrationStates(
            F, 0, 0, nullptr, CheckCleanups ? &Cleanups : nullptr);
        EXPECT_FALSE(A.Complete);
        EXPECT_FALSE(A.RegistrationLifetimeComplete);
        EXPECT_FALSE(A.CleanupFrameEffectsComplete);
        EXPECT_TRUE(A.CleanupFrameEffects.empty());
        EXPECT_TRUE(A.ChainAccesses.empty());
        ASSERT_EQ(A.Blocks.size(), F.Blocks.size());
        std::vector<int32_t> AllLevels{-1};
        for (uint32_t I = 0; I != Cxx.MaxState; ++I)
          AllLevels.push_back(I);
        for (int ID : {5, 6, BodyBlocks ? int(6 + BodyBlocks) : 4}) {
          const auto Root =
              std::find_if(A.Blocks.begin(), A.Blocks.end(),
                           [ID](const auto &B) { return B.BlockId == ID; });
          ASSERT_NE(Root, A.Blocks.end());
          EXPECT_TRUE(Root->Reached);
          EXPECT_TRUE(Root->Unknown);
          EXPECT_EQ(Root->CallbackOnly, ID == 5 || ID == 6);
          EXPECT_EQ(Root->levels(), AllLevels);
        }
        EXPECT_TRUE(std::none_of(
            A.Diagnostics.begin(), A.Diagnostics.end(), [](const auto &D) {
              return D.find("budget exhausted") != std::string::npos;
            }));
      }
}

TEST(RegistrationState, PublishedUnknownDomainsOwnTheirIndependentLifetimes) {
  std::vector<RegistrationBlockState> Retained;
  for (uint32_t Count : {2u, 9u}) {
    auto F = makeNarrowCxxStateFrame(4, 0, 0, 0);
    auto &Cxx = *F.ExceptionMetadata->Cxx;
    Cxx.MaxState = Count;
    Cxx.TryBlocks.clear();
    Cxx.UnwindMap.clear();
    for (uint32_t I = 0; I != Count; ++I)
      Cxx.UnwindMap.push_back(
          {int32_t(I) - 1, 0, CxxUnwindAction::ActionKind::None});
    F.Blocks[3].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
    const auto A = analyzeRegistrationStates(F);
    EXPECT_FALSE(A.Complete);
    EXPECT_FALSE(A.RegistrationLifetimeComplete);
    const auto Block =
        llvm::find_if(A.Blocks, [](const auto &B) { return B.BlockId == 4; });
    ASSERT_NE(Block, A.Blocks.end());
    ASSERT_TRUE(Block->Unknown);
    Retained.push_back(*Block);
  }
  // Both solvers, input functions and analysis results have been destroyed.
  // A later analysis cannot change the earlier block's complete domain.
  EXPECT_EQ(Retained[0].levels(), (std::vector<int32_t>{-1, 0, 1}));
  EXPECT_EQ(Retained[1].levels(),
            (std::vector<int32_t>{-1, 0, 1, 2, 3, 4, 5, 6, 7, 8}));
  const auto Copy = Retained;
  Retained.clear();
  EXPECT_EQ(Copy[0].levels(), (std::vector<int32_t>{-1, 0, 1}));
  EXPECT_EQ(Copy[1].levels().back(), 8);
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
      C.Calls[0].ObjectFrameOffset = -16; // SavedESP.
      break;
    case 4:
      C.Calls[0].ObjectFrameOffset = -12; // The registration link.
      break;
    case 5:
      C.Calls[0].ObjectFrameOffset = -32; // Below the allocated parent stack.
      break;
    case 6:
      C.Calls[0].ObjectFrameOffset = 0; // Saved EBP.
      break;
    case 7:
      C.Calls[0].Leaf.StackPopBytes = 4;
      break;
    case 8:
      C.Calls[0].Leaf.ECXReads[0].End = 8; // Four bytes remain uninitialized.
      break;
    case 9:
      C.Calls[0].Leaf.CallerPCWrites = {{0x3100, 0x3104}};
      break;
    case 10:
      for (auto &Op : F.Blocks[1].Ops)
        if (Op.Opcode == NdOp::STORE && Op.Inputs[0].isReg() &&
            Op.Inputs[0].Offset == x86reg::RCX)
          Op.Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      break;
    case 11:
      C.Calls[0].Leaf.ECXReads = {{4, 4}};
      break;
    }
    const auto A = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
    EXPECT_FALSE(A.CleanupFrameEffectsComplete);
    EXPECT_TRUE(A.CleanupFrameEffects.empty());
  }
}

TEST(RegistrationState, ChecksEveryCallInACombinedCleanup) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeCxxObjectCleanup();
    const std::vector Calls{objectLeafContract()};
    std::vector Cleanups{objectCleanupContract()};
    auto &Sequence = Cleanups[0].Calls;
    Sequence.push_back(Sequence[0]);
    if (Mutation == 1)
      Sequence[1].ObjectFrameOffset = -16;
    if (Mutation == 2)
      Sequence[1].Leaf.ECXReads[0].End = 8;
    if (Mutation == 3)
      Sequence.clear();
    const auto Result = analyzeRegistrationStates(F, 0, 0, &Calls, &Cleanups);
    EXPECT_EQ(Result.CleanupFrameEffectsComplete, Mutation == 0);
    if (Mutation == 0) {
      ASSERT_TRUE(Result.Complete);
      ASSERT_FALSE(Result.CleanupFrameEffects.empty());
      EXPECT_EQ(
          Result.CleanupFrameEffects[0].FrameReads,
          (std::vector<RegistrationObjectExtent>{{-24, -20}, {-24, -20}}));
    }
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
    EXPECT_TRUE(A.Blocks[2].levels().empty());
    EXPECT_TRUE(A.Blocks[0].Reached);
    EXPECT_FALSE(A.Blocks[2].Reached);
    EXPECT_TRUE(A.Blocks[5].Reached);
    EXPECT_TRUE(A.Blocks[5].CallbackOnly);
    EXPECT_TRUE(A.Blocks[6].Reached);
    ASSERT_EQ(A.CxxContinuations.size(), 1u);
    EXPECT_EQ(A.CxxContinuations[0].TargetVA, 0x1900u);
  }
}

TEST(RegistrationState, CanonicalNoReturnKeepsExceptionalFlowWithoutBorrowing) {
  for (bool Indirect : {false, true}) {
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Mutation);
      auto F = makeCxxObjectCall();
      auto &Call = F.Blocks[7];
      if (Indirect)
        Call.Ops.front().Opcode = NdOp::INDIR_CALL;
      auto &Insn = Call.InstructionBoundaries.front();
      Insn.OpCount = 1;
      Insn.ControlFlags = LowInstructionControlFlag::NoReturn;
      if (Mutation == 1)
        Insn.ControlFlags |= LowInstructionControlFlag::Conditional;
      if (Mutation == 2)
        Insn.ControlFlags = LowInstructionControlFlag::None;
      if (Mutation == 3)
        Insn.Control = LowInstructionControl::None;
      if (Mutation == 4)
        Insn.OpCount = 0;
      if (Mutation == 5)
        Call.Ops.front().Seq = -1;
      const auto A = analyzeRegistrationStates(F);
      ASSERT_TRUE(A.Complete);
      EXPECT_TRUE(A.RegistrationLifetimeComplete);
      EXPECT_FALSE(A.CallFrameEffectsComplete);
      EXPECT_TRUE(A.CallFrameEffects.empty());
      EXPECT_EQ(A.Blocks[2].Reached, Mutation != 0);
      EXPECT_TRUE(A.Blocks[5].Reached);
      EXPECT_TRUE(A.Blocks[5].CallbackOnly);
      EXPECT_TRUE(A.Blocks[6].Reached);
      ASSERT_EQ(A.CxxContinuations.size(), 1u);
      EXPECT_EQ(A.CxxContinuations[0].TargetVA, 0x1900u);
      // Requesting independent call-frame proofs still refuses the opaque
      // callee; its canonical no-return edge does not authorize a borrow.
      const std::vector<RegistrationCalleeFrameContract> Contracts;
      const auto Checked = analyzeRegistrationStates(F, 0, 0, &Contracts);
      EXPECT_FALSE(Checked.CallFrameEffectsComplete);
      EXPECT_TRUE(Checked.CallFrameEffects.empty());
      EXPECT_EQ(Checked.Blocks[2].Reached, Mutation != 0);
      EXPECT_TRUE(Checked.Blocks[5].Reached);
    }
  }
}

TEST(RegistrationState, LocalUnwindRetiresLevelsAfterCheckedFinallyEffects) {
  for (unsigned Mutation = 0; Mutation != 30; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x1800, true}};
    if (Mutation == 10)
      Chain.Scopes.front().EnclosingLevel = 0;
    if (Mutation == 11) {
      Chain.Scopes.push_back({-1, 0, 0x1900, true});
      // Actually traverse the missing finally, rather than requesting an
      // already higher stopping bound that executes no cleanup.
      Chain.TryLevelStores.front().Level = 1;
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
    }
    if (Mutation >= 20) {
      // _local_unwind2 stops at the sentinel or any level at/below its
      // unsigned threshold. It need not encounter that exact scope index.
      Chain.Scopes.push_back({-1, 0, 0x1800, true});
      const int32_t From = Mutation == 29                     ? 99
                           : Mutation == 21                   ? 1
                           : Mutation == 22 || Mutation == 27 ? -1
                                                              : 0;
      Chain.TryLevelStores.front().Level = From;
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(From), 4);
      if (Mutation == 23 || Mutation == 28) {
        Chain.Scopes.push_back({0, 0, 0x1800, true});
        Chain.TryLevelStores.front().Level = 2;
        F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(2, 4);
      }
    }
    F.Blocks.resize(7);
    auto &Setup = F.Blocks[4];
    Setup.Id = 4;
    Setup.StartAddr = 0x1080;
    Setup.EndAddr = 0x1088;
    Setup.InstructionBoundaries = {
        {0x1080, 3, 0, 1}, {0x1083, 3, 1, 1}, {0x1086, 2, 2, 1}};
    emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(32, 4)});
    emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RCX, 4), NdVar::reg(x86reg::RSP, 4)});
    F.Blocks[0].Succs = {4};
    Setup.Succs = {1};
    F.Blocks[1].Succs = {5};
    auto &Call = F.Blocks[5];
    Call.Id = 5;
    Call.StartAddr = 0x1100;
    Call.EndAddr = 0x110b;
    Call.Succs = {2};
    emitOp(Call, 0x1100, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    // The stopping bound is a numeric comparison operand, not a table index.
    // Mutation 29 keeps the independent rejection of an invalid current level.
    const auto Level =
        Mutation == 3                      ? NdVar::reg(x86reg::RAX, 4)
        : Mutation == 2                    ? NdVar::cst(99, 4)
        : Mutation == 25                   ? NdVar::cst(0x80000000u, 4)
        : Mutation == 26 || Mutation == 27 ? NdVar::cst(UINT32_MAX - 1, 4)
        : Mutation == 28                   ? NdVar::cst(0x7fffffffu, 4)
        : Mutation == 9 || Mutation == 21 || Mutation == 22 || Mutation == 24
            ? NdVar::cst(0, 4)
        : Mutation >= 20 ? NdVar::cst(1, 4)
                         : NdVar::cst(UINT32_MAX, 4);
    emitOp(Call, 0x1100, NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), Level});
    emitOp(Call, 0x1102, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBP, 4),
            NdVar::cst(uint32_t(Mutation == 1 ? -12 : -16), 4)});
    emitOp(Call, 0x1105, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1105, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::reg(x86reg::RDX, 4)});
    emitOp(Call, 0x1106, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    Call.InstructionBoundaries = {{0x1100, 2, 0, 2},
                                  {0x1102, 3, 2, 1},
                                  {0x1105, 1, 3, 2},
                                  {0x1106, 5, 5, 1}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    if (Mutation == 7)
      ++Call.EndAddr;
    if (Mutation == 13)
      Call.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    auto &Finally = F.Blocks[6];
    Finally.Id = 6;
    Finally.StartAddr = 0x1800;
    Finally.EndAddr = 0x1807;
    emitOp(Finally, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4),
            NdVar::cst(uint32_t(Mutation == 4 ? -24 : -28), 4)});
    emitOp(Finally, 0x1800, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::reg(x86reg::RBP, 4)});
    if (Mutation == 12) {
      Finally.Ops.back().Opcode = NdOp::ATOMIC_XCHG;
      Finally.Ops.back().Output = NdVar::tmp(1, 4);
    }
    if (Mutation == 16)
      Finally.Ops.back().MemoryOrdering = NdMemoryOrdering::Relaxed;
    if (Mutation == 5)
      emitOp(Finally, 0x1800, NdOp::CALL, {}, {NdVar::cst(0x2200, 4)});
    if (Mutation == 6)
      emitOp(Finally, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RBP, 4)});
    const auto Count = Finally.Ops.size();
    emitOp(Finally, 0x1806, NdOp::RETURN, {}, {});
    Finally.InstructionBoundaries = {{0x1800, 6, 0, Count},
                                     {0x1806, 1, Count, 1}};
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    if (Mutation == 8)
      Finally.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 14)
      Finally.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    if (Mutation == 15) {
      emitOp(Finally, 0x1806, NdOp::COPY, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RBP, 4)});
      ++Finally.InstructionBoundaries.back().OpCount;
    }
    if (Mutation >= 17 && Mutation < 20) {
      // Both successors return through this invocation. Join the actual
      // effects, and refuse when even one path changes the return slot.
      Finally.Ops.pop_back();
      emitOp(Finally, 0x1806, NdOp::COND_BR, {},
             {NdVar::cst(0x1820, 4), NdVar::reg(x86reg::ZF, 1)});
      Finally.InstructionBoundaries.back().Control =
          LowInstructionControl::Branch;
      Finally.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
      Finally.Succs = {7, 8};
      for (unsigned Arm = 0; Arm != 2; ++Arm) {
        LowBlock Exit;
        Exit.Id = 7 + Arm;
        Exit.StartAddr = 0x1810 + Arm * 0x10;
        Exit.EndAddr = Exit.StartAddr + 4;
        if (Arm && Mutation == 17)
          emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD,
                 NdVar::reg(x86reg::RSP, 4),
                 {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
        if (Arm && Mutation == 19) {
          emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(0, 4),
                 {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
          emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
                 {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
        }
        const size_t Prefix = Exit.Ops.size();
        emitOp(Exit, Exit.StartAddr + 3, NdOp::RETURN, {}, {});
        Exit.InstructionBoundaries = {{Exit.StartAddr, 3, 0, Prefix},
                                      {Exit.StartAddr + 3, 1, Prefix, 1}};
        Exit.InstructionBoundaries.back().Control =
            LowInstructionControl::Return;
        F.Blocks.push_back(std::move(Exit));
      }
    }
    if (Mutation == 24) {
      // One path is already at zero; the other unwinds level one to -1.
      // The shared call must retain both exits and only a may-write effect.
      LowBlock Other = F.Blocks[1];
      Other.Id = 7;
      Other.StartAddr = 0x1090;
      Other.EndAddr = 0x1097;
      Other.InstructionBoundaries.front().Address = Other.StartAddr;
      for (auto &Op : Other.Ops)
        Op.Addr = Other.StartAddr;
      Other.Ops.back().Inputs[1] = NdVar::cst(1, 4);
      Other.Succs = {5};
      F.Blocks[4].Succs = {1, 7};
      Chain.TryLevelStores.push_back({Other.StartAddr, Other.EndAddr, 1});
      F.Blocks.push_back(std::move(Other));
    }
    emitOp(F.Blocks[2], 0x1020, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    const auto ReadSeq = F.Blocks[2].Ops.back().Seq;
    emitOp(F.Blocks[2], 0x1020, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(F.Blocks[2], 0x1020, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::tmp(0, 4)});
    const auto WrittenValueSeq = F.Blocks[2].Ops.back().Seq;
    const std::vector<RegistrationLocalUnwindContract> Calls{{0x2100, false}};
    const auto A =
        analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr, &Calls);
    const bool Valid = Mutation == 0 || Mutation == 2 || Mutation == 9 ||
                       (Mutation >= 18 && Mutation != 29);
    EXPECT_EQ(A.Complete, Valid);
    EXPECT_FALSE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.CallFrameEffects.empty());
    if (Valid) {
      if (Mutation == 24) {
        EXPECT_EQ(A.Blocks[2].levels(), (std::vector<int32_t>{-1, 0}));
      } else {
        EXPECT_EQ(A.Blocks[2].levels().size(), 1u);
        if (A.Blocks[2].levels().size() != 1)
          continue;
        const int32_t ExpectedLevel =
            Mutation == 28 ? 2
            : Mutation == 2 || Mutation == 9 || Mutation == 20 ||
                    Mutation == 23 || Mutation == 25 || Mutation == 26
                ? 0
                : -1;
        EXPECT_EQ(A.Blocks[2].levels().front(), ExpectedLevel);
      }
      bool FoundSP = false;
      bool FoundFinallyWrite = false;
      for (const auto &Value : A.FrameValues)
        if (Value.Address == 0x1020 && Value.OpSeq == ReadSeq) {
          FoundSP = true;
          EXPECT_EQ(Value.EstablishedFrameOffset, -56);
        }
      for (const auto &Value : A.FrameValues)
        if (Value.Address == 0x1020 && Value.OpSeq == WrittenValueSeq) {
          FoundFinallyWrite = true;
          if (Mutation == 19 || Mutation == 24)
            EXPECT_FALSE(Value.EstablishedFrameOffset);
          else
            EXPECT_EQ(Value.EstablishedFrameOffset, 0);
        }
      EXPECT_TRUE(FoundSP);
      // A zero-step unwind must not inherit the separately analyzed runtime
      // callback's writes; a traversed finally must publish its actual effect.
      EXPECT_EQ(FoundFinallyWrite, Mutation != 2 && Mutation != 9 &&
                                       Mutation != 20 && Mutation != 22 &&
                                       Mutation < 25);
    }
  }
}

TEST(RegistrationState, OnlyCanonicalWaitPreservesLocalFinallyFrameEffects) {
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x1800, true}};
    F.Blocks.resize(7);
    auto &Setup = F.Blocks[4];
    Setup.Id = 4;
    Setup.StartAddr = 0x1080;
    Setup.EndAddr = 0x1088;
    emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(32, 4)});
    emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RCX, 4), NdVar::reg(x86reg::RSP, 4)});
    Setup.InstructionBoundaries = {
        {0x1080, 3, 0, 1}, {0x1083, 3, 1, 1}, {0x1086, 2, 2, 1}};
    F.Blocks[0].Succs = {4};
    Setup.Succs = {1};
    F.Blocks[1].Succs = {5};
    auto &Call = F.Blocks[5];
    Call.Id = 5;
    Call.StartAddr = 0x1100;
    Call.EndAddr = 0x110b;
    Call.Succs = {2};
    emitOp(Call, 0x1100, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1100, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(UINT32_MAX, 4)});
    emitOp(Call, 0x1102, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Call, 0x1105, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1105, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::reg(x86reg::RDX, 4)});
    emitOp(Call, 0x1106, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    Call.InstructionBoundaries = {{0x1100, 2, 0, 2},
                                  {0x1102, 3, 2, 1},
                                  {0x1105, 1, 3, 2},
                                  {0x1106, 5, 5, 1}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    auto &Finally = F.Blocks[6];
    Finally.Id = 6;
    Finally.StartAddr = 0x1800;
    Finally.EndAddr = 0x1808;
    emitOp(Finally, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Finally, 0x1800, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::reg(x86reg::RBP, 4)});
    emitOp(Finally, 0x1806, NdOp::INTRINSIC, {},
           {NdVar::cst(uint64_t(Intrinsic::X87Wait), 2)});
    auto &Wait = Finally.Ops.back();
    switch (Mode) {
    case 1:
      Wait.Inputs[0].Offset = uint64_t(Intrinsic::Syscall);
      break;
    case 2:
      Wait.addInput(NdVar::reg(x86reg::RBP, 4));
      break;
    case 3:
      Wait.Inputs[0].Size = 1;
      break;
    case 4:
      Wait.Output = NdVar::reg(x86reg::RAX, 4);
      break;
    case 5:
      Wait.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 6:
      Wait.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 7:
      Wait.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
      break;
    case 8:
      Wait.Inputs[0].Offset |= uint64_t{1} << 16;
      break;
    }
    emitOp(Finally, 0x1807, NdOp::RETURN, {}, {});
    Finally.InstructionBoundaries = {
        {0x1800, 6, 0, 2}, {0x1806, 1, 2, 1}, {0x1807, 1, 3, 1}};
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    emitOp(F.Blocks[2], 0x1020, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    const auto SPSeq = F.Blocks[2].Ops.back().Seq;
    emitOp(F.Blocks[2], 0x1020, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(F.Blocks[2], 0x1020, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::tmp(0, 4)});
    const auto EffectSeq = F.Blocks[2].Ops.back().Seq;
    // Close the parent registration after observing the returning finally's
    // effects; a still-installed terminal block cannot prove its lifetime.
    auto &Unlink = F.Blocks[3];
    emitOp(Unlink, 0x1030, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Unlink, 0x1030, NdOp::LOAD, NdVar::tmp(8, 4), {NdVar::tmp(0, 4)});
    emitOp(Unlink, 0x1030, NdOp::STORE, {},
           {NdVar::cst(0, 8), NdVar::tmp(8, 4)}, NdMemoryAddressSpace::X86FS);
    Unlink.InstructionBoundaries = {{0x1030, 7, 0, 3}};
    const std::vector<RegistrationLocalUnwindContract> Calls{{0x2100, false}};
    const auto A =
        analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr, &Calls);
    EXPECT_EQ(A.Complete, Mode == 0);
    EXPECT_FALSE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.CallFrameEffects.empty());
    if (Mode != 0)
      continue;
    EXPECT_TRUE(A.CallbackStatesComplete);
    EXPECT_TRUE(A.RegistrationLifetimeComplete);
    ASSERT_EQ(A.Blocks[2].levels(), (std::vector<int32_t>{-1}));
    bool FoundSP = false, FoundEffect = false;
    for (const auto &Value : A.FrameValues) {
      if (Value.Address == 0x1020 && Value.OpSeq == SPSeq) {
        FoundSP = true;
        EXPECT_EQ(Value.EstablishedFrameOffset, -56);
      }
      if (Value.Address == 0x1020 && Value.OpSeq == EffectSeq) {
        FoundEffect = true;
        EXPECT_EQ(Value.EstablishedFrameOffset, 0);
      }
    }
    EXPECT_TRUE(FoundSP);
    EXPECT_TRUE(FoundEffect);
    EXPECT_TRUE(A.Blocks[6].Reached);
    EXPECT_TRUE(A.Blocks[6].CanDispatch);
  }
}

TEST(RegistrationState, LocalFinallyOwnsOnlyItsAllocatedCallbackStack) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x1800, true}};
    F.Blocks.resize(7);
    auto &Setup = F.Blocks[4];
    Setup.Id = 4;
    Setup.StartAddr = 0x1080;
    Setup.EndAddr = 0x1088;
    emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(32, 4)});
    emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RCX, 4), NdVar::reg(x86reg::RSP, 4)});
    Setup.InstructionBoundaries = {
        {0x1080, 3, 0, 1}, {0x1083, 3, 1, 1}, {0x1086, 2, 2, 1}};
    F.Blocks[0].Succs = {4};
    Setup.Succs = {1};
    F.Blocks[1].Succs = {5};
    auto &Call = F.Blocks[5];
    Call.Id = 5;
    Call.StartAddr = 0x1100;
    Call.EndAddr = 0x110b;
    Call.Succs = {2};
    emitOp(Call, 0x1100, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1100, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(UINT32_MAX, 4)});
    emitOp(Call, 0x1102, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
    emitOp(Call, 0x1105, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Call, 0x1105, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::reg(x86reg::RDX, 4)});
    emitOp(Call, 0x1106, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
    Call.InstructionBoundaries = {{0x1100, 2, 0, 2},
                                  {0x1102, 3, 2, 1},
                                  {0x1105, 1, 3, 2},
                                  {0x1106, 5, 5, 1}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;

    auto &Finally = F.Blocks[6];
    Finally.Id = 6;
    Finally.StartAddr = 0x1800;
    // Each operation gets an exact source occurrence, including the return.
    va_t Address = Finally.StartAddr;
    auto Emit = [&](NdOp Opcode, NdVar Output,
                    std::initializer_list<NdVar> Inputs) {
      const size_t First = Finally.Ops.size();
      emitOp(Finally, Address, Opcode, Output, Inputs);
      Finally.InstructionBoundaries.push_back({Address++, 1, First, 1});
    };
    const auto SP = NdVar::reg(x86reg::RSP, 4);
    const auto BP = NdVar::reg(x86reg::RBP, 4);
    const auto Saved = NdVar::reg(x86reg::RSI, 4);
    // Save the parent's EBP in this invocation, use a private frame pointer,
    // then recover EBP before touching a parent local. Neither stack's -4
    // slot may be mistaken for the other's storage.
    Emit(NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
    if (Mutation != 1)
      Emit(NdOp::STORE, {},
           {SP, Mutation == 4 ? NdVar::reg(x86reg::RBP, 2) : BP});
    Emit(NdOp::COPY, Saved, {SP});
    Emit(NdOp::COPY, BP, {SP});
    Emit(NdOp::INT_SUB, SP, {SP, NdVar::cst(8, 4)});
    if (Mutation == 2 || Mutation == 3)
      Emit(NdOp::INT_ADD, Saved,
           {Saved, NdVar::cst(Mutation == 2 ? uint32_t(-12) : 4, 4)});
    if (Mutation == 5) {
      // Release and reallocate the same bytes: their old initialization and
      // saved-register identity no longer belong to the new allocation.
      Emit(NdOp::INT_ADD, SP, {SP, NdVar::cst(12, 4)});
      Emit(NdOp::INT_SUB, SP, {SP, NdVar::cst(12, 4)});
    }
    Emit(NdOp::LOAD, Mutation == 7 ? NdVar::reg(x86reg::RBP, 2) : BP, {Saved});
    Emit(NdOp::INT_ADD, Saved,
         {BP, NdVar::cst(uint32_t(Mutation == 8 ? -24 : -28), 4)});
    Emit(NdOp::STORE, {}, {Saved, Mutation == 9 ? SP : BP});
    if (Mutation == 10)
      Emit(NdOp::CALL, {}, {NdVar::cst(0x2200, 4)});
    Emit(NdOp::INT_ADD, SP, {SP, NdVar::cst(Mutation == 6 ? 8 : 12, 4)});
    Emit(NdOp::RETURN, {}, {});
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    Finally.EndAddr = Address;
    emitOp(F.Blocks[2], 0x1020, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {BP, NdVar::cst(uint32_t(-28), 4)});
    emitOp(F.Blocks[2], 0x1020, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
           {NdVar::tmp(0, 4)});
    const int ReadSeq = F.Blocks[2].Ops.back().Seq;
    const std::vector<RegistrationLocalUnwindContract> Calls{{0x2100, false}};
    const auto A =
        analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr, &Calls);
    EXPECT_EQ(A.Complete, Mutation == 0);
    EXPECT_FALSE(A.CallFrameEffectsComplete);
    if (Mutation == 0) {
      EXPECT_TRUE(A.CallbackStatesComplete);
      ASSERT_EQ(A.Blocks[2].levels(), std::vector<int32_t>{-1});
      const auto Written = llvm::find_if(A.FrameValues, [&](const auto &Value) {
        return Value.Address == 0x1020 && Value.OpSeq == ReadSeq;
      });
      ASSERT_NE(Written, A.FrameValues.end());
      EXPECT_EQ(Written->EstablishedFrameOffset, 0);
    }
  }
}

TEST(RegistrationState, SEHCallbacksMustReturnToTheirCurrentInvocation) {
  for (bool EH4 : {false, true}) {
    for (bool Finally : {false, true}) {
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Finally);
        SCOPED_TRACE(Mutation);
        auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
        auto &Scope = F.ExceptionMetadata->Registration->Scopes.front();
        Scope.FilterVA = Finally ? 0 : 0x1800;
        Scope.HandlerVA = Finally ? 0x1800 : 0x1900;
        Scope.IsFinally = Finally;
        LowBlock Callback;
        Callback.Id = F.Blocks.size();
        Callback.StartAddr = 0x1800;
        emitOp(Callback, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
               {NdVar::cst(1, 4)});
        if (Mutation == 1)
          emitOp(Callback, 0x1800, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
                 {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
        if (Mutation == 2)
          emitOp(Callback, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RBP, 4),
                 {NdVar::reg(x86reg::RSP, 4)});
        const size_t Prefix = Callback.Ops.size();
        emitOp(Callback, 0x1801, NdOp::RETURN, {}, {});
        if (Mutation == 7)
          Callback.Ops.back().Seq = -1;
        if (Mutation == 5)
          emitOp(Callback, 0x1801, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                 {NdVar::cst(2, 4)});
        Callback.InstructionBoundaries = {
            {0x1800, 1, 0, Prefix},
            {0x1801, 1, Prefix, Callback.Ops.size() - Prefix}};
        auto &Return = Callback.InstructionBoundaries.back();
        Return.Control = Mutation == 8 ? LowInstructionControl::Call
                                       : LowInstructionControl::Return;
        if (Mutation == 3)
          Return.Immediate = 4;
        if (Mutation == 4)
          Return.ControlFlags = LowInstructionControlFlag::Conditional;
        if (Mutation == 6)
          Callback.InstructionBoundaries.pop_back();
        Callback.EndAddr = 0x1802;
        const size_t CallbackIndex = F.Blocks.size();
        F.Blocks.push_back(std::move(Callback));
        const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0);
        EXPECT_TRUE(A.Complete);
        EXPECT_EQ(A.CallbackStatesComplete, Mutation == 0);
        ASSERT_GT(A.Blocks.size(), CallbackIndex);
        EXPECT_TRUE(A.Blocks[CallbackIndex].Reached);
        EXPECT_TRUE(A.Blocks[CallbackIndex].CallbackOnly);
        EXPECT_EQ(A.Blocks[CallbackIndex].Unknown, Mutation != 0);
      }
    }
  }
}

TEST(RegistrationState, SharedSEHCallbackTailsKeepEachInvocationStack) {
  for (bool EH4 : {false, true}) {
    for (bool Finally : {false, true}) {
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(EH4);
          SCOPED_TRACE(Finally);
          SCOPED_TRACE(Mutation);
          SCOPED_TRACE(Reverse);
          auto F = EH4 ? makeCookieFrame() : makeBranchingFrame();
          auto &Chain = *F.ExceptionMetadata->Registration;
          const int32_t Outside = EH4 ? -2 : -1;
          Chain.Scopes = {{Outside, Finally ? 0u : 0x1800u,
                           Finally ? 0x1800u : 0x1900u, Finally},
                          {0, Finally ? 0u : 0x1820u,
                           Finally ? 0x1820u : 0x1900u, Finally}};
          if (Reverse) {
            std::swap(Chain.Scopes[0].FilterVA, Chain.Scopes[1].FilterVA);
            std::swap(Chain.Scopes[0].HandlerVA, Chain.Scopes[1].HandlerVA);
          }
          Chain.TryLevelStores.front().Level = 1;
          F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
          F.Blocks[0].Succs = {1};
          const auto SP = NdVar::reg(x86reg::RSP, 4);
          const auto BP = NdVar::reg(x86reg::RBP, 4);
          for (unsigned Entry = 0; Entry != 2; ++Entry) {
            LowBlock Callback;
            Callback.Id = 4 + Entry;
            Callback.StartAddr = 0x1800 + Entry * 0x20;
            va_t Address = Callback.StartAddr;
            auto Emit = [&](NdOp Opcode, NdVar Output,
                            std::initializer_list<NdVar> Inputs) {
              const size_t First = Callback.Ops.size();
              emitOp(Callback, Address, Opcode, Output, Inputs);
              Callback.InstructionBoundaries.push_back(
                  {Address++, 1, First, 1});
            };
            Emit(NdOp::INT_SUB, SP,
                 {SP, NdVar::cst(Entry && Mutation == 1 ? 8 : 4, 4)});
            if (!(Entry && Mutation == 3))
              Emit(NdOp::STORE, {}, {SP, BP});
            if (Entry && Mutation == 2) {
              Emit(NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
                   {SP, NdVar::cst(4, 4)});
              Emit(NdOp::STORE, {},
                   {NdVar::reg(x86reg::RCX, 4), NdVar::cst(0, 4)});
            }
            Emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                 {Entry ? NdVar::cst(0, 4) : BP});
            if (Entry && Mutation == 5)
              Emit(NdOp::COPY, BP, {SP});
            Emit(NdOp::BRANCH, {}, {NdVar::cst(0x1840, 4)});
            Callback.InstructionBoundaries.back().Control =
                LowInstructionControl::Branch;
            Callback.EndAddr = Address;
            Callback.Succs = {6};
            F.Blocks.push_back(std::move(Callback));
          }
          LowBlock Tail;
          Tail.Id = 6;
          Tail.StartAddr = 0x1840;
          Tail.EndAddr = 0x1844;
          Tail.Preds = {4, 5};
          emitOp(Tail, 0x1840, NdOp::COPY, NdVar::reg(x86reg::RCX, 4),
                 {NdVar::reg(x86reg::RAX, 4)});
          emitOp(Tail, 0x1841, NdOp::LOAD, BP, {SP});
          emitOp(Tail, 0x1842, NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
          emitOp(Tail, 0x1843, NdOp::RETURN, {}, {});
          Tail.InstructionBoundaries = {{0x1840, 1, 0, 1},
                                        {0x1841, 1, 1, 1},
                                        {0x1842, 1, 2, 1},
                                        {0x1843, 1, 3, 1}};
          Tail.InstructionBoundaries.back().Control =
              LowInstructionControl::Return;
          if (Mutation == 6)
            Tail.InstructionBoundaries.back().Immediate = 4;
          if (Mutation == 4) {
            F.Blocks[3].Succs = {6};
            Tail.Preds.push_back(3);
          }
          F.Blocks.push_back(std::move(Tail));
          if (Reverse)
            std::swap(F.Blocks[4], F.Blocks[5]);
          const auto A = analyzeRegistrationStates(F, EH4 ? 0x4000 : 0);
          EXPECT_EQ(A.Complete, Mutation != 4);
          EXPECT_EQ(A.CallbackStatesComplete, Mutation == 0 || Mutation == 5);
          ASSERT_EQ(A.Blocks.size(), F.Blocks.size());
          EXPECT_TRUE(A.Blocks.back().Reached);
          EXPECT_EQ(A.Blocks.back().CallbackOnly, Mutation != 4);
          EXPECT_EQ(A.Blocks.back().Unknown, Mutation != 0 && Mutation != 5);
          if (Mutation == 0 || Mutation == 5) {
            // A clobbered BP is restored from this invocation's private slot.
            // No callback may borrow another entry's stack initialization.
            const auto Restored =
                llvm::find_if(A.FrameValues, [](const auto &V) {
                  return V.Address == 0x1841 && V.OpSeq == 1;
                });
            ASSERT_NE(Restored, A.FrameValues.end());
            EXPECT_EQ(Restored->EstablishedFrameOffset, 0);
            const auto Mixed = llvm::find_if(A.FrameValues, [](const auto &V) {
              return V.Address == 0x1840 && V.OpSeq == 0;
            });
            ASSERT_NE(Mixed, A.FrameValues.end());
            EXPECT_FALSE(Mixed->EstablishedFrameOffset);
          }
        }
      }
    }
  }
}

TEST(RegistrationState, SEHActivationsShareTheCumulativeWorkBudget) {
  for (auto [Count, TailOps] :
       {std::pair{2u, 2048u}, std::pair{512u, 0u}, std::pair{512u, 2048u}}) {
    SCOPED_TRACE(Count);
    SCOPED_TRACE(TailOps);
    auto F = makeBranchingFrame();
    F.ExceptionMetadata->CodeRange.End = 0x40000;
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes.clear();
    Chain.TryLevelStores.front().Level = Count - 1;
    F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(Count - 1, 4);
    F.Blocks[0].Succs = {1};
    const int TailId = 4 + Count;
    for (unsigned Entry = 0; Entry != Count; ++Entry) {
      const va_t Address = 0x1800 + Entry * 4;
      Chain.Scopes.push_back({int32_t(Entry) - 1, 0, Address, true});
      LowBlock Callback;
      Callback.Id = 4 + Entry;
      Callback.StartAddr = Address;
      Callback.EndAddr = Address + 1;
      emitOp(Callback, Address, NdOp::BRANCH, {}, {NdVar::cst(0x30000, 4)});
      Callback.InstructionBoundaries = {{Address, 1, 0, 1}};
      Callback.InstructionBoundaries.back().Control =
          LowInstructionControl::Branch;
      Callback.Succs = {TailId};
      F.Blocks.push_back(std::move(Callback));
    }
    LowBlock Tail;
    Tail.Id = TailId;
    Tail.StartAddr = 0x30000;
    Tail.EndAddr = 0x30001 + TailOps;
    // The shared tail executes in every callback activation. Its operations
    // alone exceed the cumulative budget in the large nonempty case.
    for (unsigned I = 0; I != TailOps; ++I) {
      emitOp(Tail, Tail.StartAddr + I, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
             {NdVar::reg(x86reg::RAX, 4)});
      Tail.InstructionBoundaries.push_back({Tail.StartAddr + I, 1, I, 1});
    }
    emitOp(Tail, Tail.EndAddr - 1, NdOp::RETURN, {}, {});
    Tail.InstructionBoundaries.push_back({Tail.EndAddr - 1, 1, TailOps, 1});
    Tail.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    for (unsigned Entry = 0; Entry != Count; ++Entry)
      Tail.Preds.push_back(4 + Entry);
    F.Blocks.push_back(std::move(Tail));
    const auto A = analyzeRegistrationStates(F);
    if (Count == 2 || !TailOps) {
      EXPECT_TRUE(A.Complete);
      EXPECT_TRUE(A.CallbackStatesComplete);
    } else {
      EXPECT_FALSE(A.Complete);
      EXPECT_TRUE(A.Blocks.empty());
      EXPECT_TRUE(A.FrameValues.empty());
      EXPECT_FALSE(A.SEHInvocationExitsComplete);
      EXPECT_TRUE(A.SEHInvocationExits.empty());
      EXPECT_FALSE(A.SEHExceptionAccessesComplete);
      EXPECT_TRUE(A.SEHExceptionAccesses.empty());
      EXPECT_TRUE(llvm::any_of(A.Diagnostics, [](const auto &D) {
        return D.find("budget exhausted") != std::string::npos;
      }));
    }
  }
}

TEST(RegistrationState, OnlyCanonicalWaitPreservesInitializedFrameObjects) {
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto F = makeCxxObjectCall();
    LowBlock Effects;
    emitOp(Effects, 0x1013, NdOp::INTRINSIC, {},
           {NdVar::cst(uint64_t(Intrinsic::X87Wait), 2)});
    auto &Wait = Effects.Ops.back();
    switch (Mode) {
    case 1:
      Wait.Inputs[0].Offset = uint64_t(Intrinsic::Syscall);
      break;
    case 2:
      Wait.addInput(NdVar::reg(x86reg::RBP, 4));
      break;
    case 3:
      Wait.Inputs[0].Size = 1;
      break;
    case 4:
      Wait.Output = NdVar::reg(x86reg::RAX, 4);
      break;
    case 5:
      Wait.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 6:
      Wait.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 7:
      Wait.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
      break;
    case 8:
      Wait.Inputs[0].Offset |= uint64_t{1} << 16;
      break;
    }
    auto &Body = F.Blocks[1];
    auto BeforeState =
        std::find_if(Body.Ops.begin(), Body.Ops.end(),
                     [](const auto &Op) { return Op.Addr == 0x1016; });
    ASSERT_NE(BeforeState, Body.Ops.end());
    Body.Ops.insert(BeforeState, Effects.Ops.begin(), Effects.Ops.end());
    for (size_t I = 0; I != Body.Ops.size(); ++I)
      Body.Ops[I].Seq = I;
    const std::vector Contracts{objectLeafContract()};
    const auto A = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(A.Complete, Mode == 0);
    EXPECT_EQ(A.CallFrameEffectsComplete, Mode == 0);
    if (Mode == 0) {
      EXPECT_TRUE(A.RegistrationLifetimeComplete);
      EXPECT_TRUE(A.CxxContinuationsComplete);
      ASSERT_EQ(A.CallFrameEffects.size(), 1u);
      EXPECT_EQ(A.CallFrameEffects[0].FrameReads,
                (std::vector<RegistrationObjectExtent>{{-24, -20}}));
    } else
      EXPECT_TRUE(A.CallFrameEffects.empty());
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

TEST(RegistrationState, APossibleCalleeWriteCannotLaunderTheObjectPointer) {
  for (int32_t End : {1, 4}) {
    SCOPED_TRACE(End);
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
    Writer.ECXWrites = {{0, End}};
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
  EXPECT_TRUE(Result.Blocks.back().levels().empty());
  EXPECT_FALSE(Result.Blocks.back().CanDispatch);
}

LowFunc makePopulatedFrame(bool Exact) {
  auto F = makeUnlinkedFrame();
  F.ExceptionMetadata->CodeRange.End = 0x10000;
  LowBlock Body;
  Body.Id = F.Blocks.size();
  Body.StartAddr = 0x2000;
  Body.Succs = F.Blocks.front().Succs;
  F.Blocks.front().Succs = {Body.Id};
  va_t Address = Body.StartAddr;
  auto Emit = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs) {
    const unsigned First = Body.Ops.size();
    emitOp(Body, Address, Opcode, Output, Inputs);
    Body.InstructionBoundaries.push_back({Address++, 1, First, 1});
  };
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto Slot = NdVar::reg(x86reg::RDI, 4);
  Emit(NdOp::INT_SUB, SP, {SP, NdVar::cst(4096, 4)});
  constexpr int32_t PopulatedCells = 512;
  for (int32_t I = 0; I != PopulatedCells; ++I) {
    Emit(NdOp::INT_ADD, Slot,
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32 - I * 4), 4)});
    Emit(NdOp::STORE, {}, {Slot, NdVar::cst(uint32_t(I), 4)});
  }
  // Thousands of exact accesses to a populated frame must leave enough of
  // the same cumulative budget to prove the subsequent registration unlink.
  // Each load still reads the current cell, including the last overwrite.
  const size_t Loads =
      Exact ? 4096
            : limits::kMaxRegistrationEHStateWork / size_t(PopulatedCells) + 1;
  for (size_t I = 0; I != Loads; ++I)
    Emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
         {Exact ? Slot : NdVar::reg(x86reg::RCX, 4)});
  Emit(NdOp::INT_ADD, SP, {SP, NdVar::cst(4096, 4)});
  Body.EndAddr = Address;
  F.Blocks.push_back(std::move(Body));
  return F;
}

TEST(RegistrationState, ExactLocalAccessesDoNotRescanTheWholeFrame) {
  const auto Result = analyzeRegistrationStates(makePopulatedFrame(true));
  EXPECT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_TRUE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.Diagnostics.empty());
}

TEST(RegistrationState, UnknownLocalAccessesRetainTheWholeFrameWorkLimit) {
  const auto Result = analyzeRegistrationStates(makePopulatedFrame(false));
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.FrameValues.empty());
  EXPECT_NE(std::find(Result.Diagnostics.begin(), Result.Diagnostics.end(),
                      "registration-state propagation budget exhausted"),
            Result.Diagnostics.end());
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
  EXPECT_EQ(Result.Blocks[4].levels(), (std::vector<int32_t>{0}));
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
  EXPECT_TRUE(A.Blocks[0].levels().empty());
  EXPECT_FALSE(A.Blocks.back().Reached);
  EXPECT_TRUE(A.Blocks.back().levels().empty());
  EXPECT_FALSE(A.Blocks.back().Unknown);
}

//===- RegistrationCxxCookieStateTests.cpp - C++ cookie conditions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

struct CxxCookieFrame {
  LowFunc Function = makeCxxCatchContinuation();
  coff_loader::X86CxxHandlerABI Handler;
  std::vector<RegistrationCalleeFrameContract> Callees;
  std::vector<RegistrationCleanupFrameContract> Cleanups;
  std::vector<size_t> Initializers;

  CxxCookieFrame(unsigned Count, int32_t XOROffset, bool ExitCheck = false) {
    Handler.SecurityCookieVA = 0x4000;
    auto &Entry = Function.Blocks.front();
    auto Install = Entry.Ops.back();
    Entry.Ops.pop_back();
    emitOp(Entry, 0x1000, NdOp::COPY, NdVar::tmp(106, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    emitOp(Entry, 0x1000, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(28, 4)});
    for (unsigned I = 0; I != Count; ++I) {
      const int32_t Slot = -20 - 4 * I;
      const int32_t XOR = XOROffset + 12 * I;
      Handler.CookieGuards.push_back(
          {Slot + 12, XOR + 12, 0x3000 + I * 16, 0x5000, 0x5018});
      emitOp(Entry, 0x1000, NdOp::LOAD, NdVar::tmp(100, 4),
             {NdVar::cst(0x4000, 4)});
      emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(101, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(XOR), 4)});
      emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(102, 4),
             {NdVar::tmp(100, 4), NdVar::tmp(101, 4)});
      emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(103, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Slot), 4)});
      Initializers.push_back(Entry.Ops.size());
      emitOp(Entry, 0x1000, NdOp::STORE, {},
             {NdVar::tmp(103, 4), NdVar::tmp(102, 4)});
    }
    Install.Seq = Entry.Ops.size();
    Install.Inputs[1] = NdVar::tmp(106, 4);
    Entry.Ops.push_back(Install);
    if (!ExitCheck)
      return;
    auto &Exit = Function.Blocks[3];
    const auto Unlink = Exit.Ops;
    Exit.Ops.clear();
    Exit.EndAddr += 5;
    Exit.InstructionBoundaries = {{0x1030, 5}, {0x1035, 7}};
    Exit.InstructionBoundaries.front().Control = LowInstructionControl::Call;
    emitOp(Exit, 0x1030, NdOp::INT_ADD, NdVar::tmp(104, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-20), 4)});
    emitOp(Exit, 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::tmp(104, 4)});
    emitOp(Exit, 0x1030, NdOp::INT_ADD, NdVar::tmp(105, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(XOROffset), 4)});
    emitOp(Exit, 0x1030, NdOp::INT_XOR, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RCX, 4), NdVar::tmp(105, 4)});
    emitOp(Exit, 0x1030, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
           {NdVar::cst(0x5000, 4)});
    for (auto Op : Unlink) {
      Op.Addr += 5;
      Op.Seq = Exit.Ops.size();
      Exit.Ops.push_back(Op);
    }
  }

  RegistrationStateAnalysis analyze() const {
    return analyzeRegistrationStates(Function, 0x4000, 0, &Callees, &Cleanups,
                                     nullptr, nullptr, nullptr, &Handler);
  }
};

TEST(RegistrationCxxCookies, ProvesSingleAndDoubleNodeConditionsAcrossCatch) {
  for (unsigned Count : {1u, 2u})
    for (int32_t XOR : {0, -16})
      for (bool ExitCheck : {false, true}) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(XOR);
        SCOPED_TRACE(ExitCheck);
        CxxCookieFrame F(Count, XOR, ExitCheck);
        const auto Result = F.analyze();
        ASSERT_TRUE(Result.Complete);
        EXPECT_TRUE(Result.CallFrameEffectsComplete);
        EXPECT_TRUE(Result.CleanupFrameEffectsComplete);
        ASSERT_TRUE(Result.SecurityCookiesComplete);
        ASSERT_EQ(Result.CxxCookieConditions.size(), Count);
        EXPECT_EQ(Result.SecurityCookieVA, 0x4000u);
        EXPECT_EQ(Result.CookieCheckVA, 0x5000u);
        for (unsigned I = 0; I != Count; ++I) {
          EXPECT_EQ(Result.CxxCookieConditions[I].FrameOffset,
                    -20 - 4 * int32_t(I));
          EXPECT_EQ(Result.CxxCookieConditions[I].XORFrameOffset,
                    XOR + 12 * int32_t(I));
        }
        EXPECT_EQ(Result.CookieChecks.size(), ExitCheck ? 1u : 0u);
        EXPECT_FALSE(analyzeRegistrationStates(F.Function, 0x4000, 0,
                                               &F.Callees, &F.Cleanups)
                         .SecurityCookiesComplete);
      }
}

TEST(RegistrationCxxCookies, RejectsMissingChangedAndAliasedConditions) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxCookieFrame F(2, 0, true);
    auto &Entry = F.Function.Blocks.front();
    const size_t Init = F.Initializers.back();
    switch (Mutation) {
    case 0:
      Entry.Ops[Init].Inputs[1].Size = 1;
      break;
    case 1:
      Entry.Ops[Init].Inputs[1] = NdVar::cst(0, 4);
      break;
    case 2:
      Entry.Ops[Init - 2].Opcode = NdOp::INT_ADD;
      break;
    case 3:
      Entry.Ops[Init - 4].Inputs[0] = NdVar::cst(0x4004, 4);
      break;
    case 4:
      ++F.Handler.CookieGuards.back().XOROffset;
      break;
    case 5:
      F.Handler.CookieGuards.back().CookieOffset = -4; // SavedESP
      break;
    case 6:
      F.Handler.CookieGuards.back().CookieOffset = 8; // TryLevel
      break;
    case 7:
      F.Handler.CookieGuards.back().CookieOffset = INT32_MIN;
      break;
    case 8:
      F.Handler.CookieGuards.back().CheckVA += 16;
      break;
    case 9:
      F.Handler.SecurityCookieVA += 4;
      break;
    case 10:
      F.Function.Blocks[3].Ops[3].Opcode = NdOp::INT_ADD;
      break;
    case 11:
      F.Function.Blocks[3].InstructionBoundaries.front().Control =
          LowInstructionControl::None;
      break;
    }
    const auto Result = F.analyze();
    EXPECT_FALSE(Result.SecurityCookiesComplete);
    EXPECT_FALSE(Result.CallFrameEffectsComplete);
    EXPECT_FALSE(Result.RuntimeObjectAccessesComplete);
    EXPECT_FALSE(Result.ImageReadsComplete);
    EXPECT_TRUE(Result.CxxCookieConditions.empty());
    EXPECT_TRUE(Result.CookieChecks.empty());
  }
}

TEST(RegistrationCxxCookies, LiveStoresCannotChangeOrPartiallyOverlapACookie) {
  for (int32_t Offset : {-24, -23, -21, -20, -17}) {
    SCOPED_TRACE(Offset);
    CxxCookieFrame F(2, 0);
    auto &Body = F.Function.Blocks[1];
    // Insert a full ordinary source store before the next try-level write.
    LowBlock Write;
    emitOp(Write, 0x1013, NdOp::INT_ADD, NdVar::tmp(110, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
    emitOp(Write, 0x1013, NdOp::STORE, {},
           {NdVar::tmp(110, 4), NdVar::cst(0, 4)});
    Body.Ops.insert(Body.Ops.end() - 2, Write.Ops.begin(), Write.Ops.end());
    const auto Result = F.analyze();
    EXPECT_FALSE(Result.SecurityCookiesComplete);
    EXPECT_TRUE(Result.CxxCookieConditions.empty());
  }
}

TEST(RegistrationCxxCookies, CallsNeedDisjointImageAndFrameWriteEffects) {
  // Unknown callees, image-cookie writes and whole/partial cookie borrows
  // cannot preserve the conditions. Adjacent private object writes can.
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxCookieFrame F(2, 0, true);
    LowBlock Call;
    Call.Id = 7;
    Call.StartAddr = 0x1700;
    Call.EndAddr = 0x1705;
    Call.InstructionBoundaries = {{0x1700, 5}};
    Call.InstructionBoundaries.front().Control = LowInstructionControl::Call;
    Call.Succs = {1};
    const int32_t Object = Mutation == 2 ? -20 : Mutation == 3 ? -23 : -32;
    emitOp(Call, 0x1700, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Object), 4)});
    emitOp(Call, 0x1700, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
           {NdVar::cst(0x6000, 4)});
    F.Function.Blocks.front().Succs = {7};
    F.Function.Blocks.push_back(std::move(Call));
    if (Mutation != 0) {
      auto &Callee = F.Callees.emplace_back();
      Callee.Target = 0x6000;
      if (Mutation == 1)
        Callee.ImageWrites.push_back({0x4000, 0x4004});
      if (Mutation >= 2 && Mutation <= 4)
        Callee.ECXWrites.push_back({0, 4});
      if (Mutation == 5)
        Callee.ImageWrites.push_back({0x4004, 0x4008});
      if (Mutation == 6)
        Callee.ImageWrites.push_back({0x4003, 0x4005});
      if (Mutation == 7)
        Callee.ImageWrites.push_back({0x3ffc, 0x4000});
    }
    const auto Result = F.analyze();
    const bool Preserved = Mutation == 4 || Mutation == 5 || Mutation == 7;
    EXPECT_EQ(Result.SecurityCookiesComplete, Preserved);
    EXPECT_EQ(Result.CallFrameEffectsComplete, Preserved);
    EXPECT_EQ(Result.CxxCookieConditions.empty(), !Preserved);
  }
}

TEST(RegistrationCxxCookies, EveryPredecessorMustInitializeBothConditions) {
  for (bool BothArms : {false, true}) {
    SCOPED_TRACE(BothArms);
    CxxCookieFrame F(2, -16);
    auto &Entry = F.Function.Blocks.front();
    const size_t Begin = F.Initializers.front() - 4;
    std::vector<LowOp> Initialization(Entry.Ops.begin() + Begin,
                                      Entry.Ops.end() - 1);
    Entry.Ops.resize(Begin);
    Entry.EndAddr = 0x1001;
    Entry.InstructionBoundaries = {{0x1000, 1}};
    Entry.Succs = {7, 8};
    for (int Arm = 0; Arm != 2; ++Arm) {
      LowBlock Block;
      Block.Id = 7 + Arm;
      Block.StartAddr = 0x1001 + Arm;
      Block.EndAddr = Block.StartAddr + 1;
      Block.InstructionBoundaries = {{Block.StartAddr, 1}};
      Block.Succs = {9};
      if (Arm == 0 || BothArms)
        Block.Ops = Initialization;
      else
        // This path initializes only the first of the two guards.
        Block.Ops.assign(Initialization.begin(), Initialization.begin() + 5);
      for (auto &Op : Block.Ops)
        Op.Addr = Block.StartAddr;
      F.Function.Blocks.push_back(std::move(Block));
    }
    LowBlock Install;
    Install.Id = 9;
    Install.StartAddr = 0x1003;
    Install.EndAddr = 0x100a;
    Install.InstructionBoundaries = {{0x1003, 7}};
    Install.Succs = {1};
    emitOp(Install, 0x1003, NdOp::INT_ADD, NdVar::tmp(120, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-12), 4)});
    emitOp(Install, 0x1003, NdOp::STORE, {},
           {NdVar::cst(0, 4), NdVar::tmp(120, 4)}, NdMemoryAddressSpace::X86FS);
    F.Function.ExceptionMetadata->Registration->ChainInstallVA = 0x1003;
    F.Function.Blocks.push_back(std::move(Install));
    const auto Result = F.analyze();
    EXPECT_TRUE(Result.Complete);
    EXPECT_EQ(Result.SecurityCookiesComplete, BothArms);
    EXPECT_EQ(Result.CxxCookieConditions.size(), BothArms ? 2u : 0u);
  }
}

TEST(RegistrationCxxCookies, CleanupWritesMustPreserveBothEncodedWords) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxCookieFrame F(2, 0, true);
    F.Function.ExceptionMetadata->Cxx->UnwindMap[0] = {
        -1, 0x2200, CxxUnwindAction::ActionKind::Direct};
    auto &Cleanup = F.Cleanups.emplace_back();
    Cleanup.ActionState = 0;
    Cleanup.RelayTarget = 0x2200;
    auto &Call = Cleanup.Calls.emplace_back();
    Call.ObjectFrameOffset = Mutation == 1 ? -20 : Mutation == 2 ? -23 : -32;
    Call.Leaf.Target = 0x6000;
    Call.Leaf.ECXWrites = {{0, 4}};
    if (Mutation == 3)
      Call.Leaf.ImageWrites = {{0x4000, 0x4004}};
    if (Mutation == 4)
      F.Cleanups.clear();
    if (Mutation == 5)
      Call.Leaf.ImageWrites = {{0x4004, 0x4008}};
    const auto Result = F.analyze();
    const bool Preserved = Mutation == 0 || Mutation == 5;
    EXPECT_EQ(Result.SecurityCookiesComplete, Preserved);
    EXPECT_EQ(Result.CxxCookieConditions.size(), Preserved ? 2u : 0u);
    if (Preserved) {
      ASSERT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.CleanupFrameEffectsComplete);
      EXPECT_FALSE(Result.CleanupFrameEffects.empty());
      EXPECT_EQ(Result.CookieChecks.size(), 1u);
    }
  }
}

TEST(RegistrationCxxCookies, CatchObjectConstructionCannotOverwriteConditions) {
  for (bool Reference : {false, true})
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Reference);
      SCOPED_TRACE(Mutation);
      CxxCookieFrame F(2, -16, true);
      auto &Catch = F.Function.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0];
      Catch.TypeDescriptorVA = 0x7000;
      Catch.CatchObjectOffset = Mutation == 1 ? -20 : Mutation == 2 ? -23 : -28;
      Catch.Adjectives = Reference ? 8 : 0;
      if (Mutation != 3) {
        auto &Throw = F.Callees.emplace_back();
        Throw.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
        Throw.Target = 0x6000;
        Throw.DoesNotReturn = true;
        Throw.ThrownTypeVA = 0x7000;
        Throw.ThrownObjectSize = 4;
      }
      const auto Result = F.analyze();
      EXPECT_EQ(Result.SecurityCookiesComplete, Mutation == 0);
      EXPECT_EQ(Result.CxxCookieConditions.size(), Mutation == 0 ? 2u : 0u);
      if (Mutation == 0) {
        EXPECT_TRUE(Result.CxxCatchObjectsComplete);
        EXPECT_EQ(Result.CxxCatchObjects.size(), 1u);
        EXPECT_EQ(Result.CookieChecks.size(), 1u);
      }
    }
}

TEST(RegistrationCxxCookies, CallsCannotReuseADeallocatedCookieSlot) {
  for (bool Checker : {false, true})
    for (bool Deallocated : {false, true}) {
      SCOPED_TRACE(Checker);
      SCOPED_TRACE(Deallocated);
      CxxCookieFrame F(2, 0, true);
      auto &Exit = F.Function.Blocks[3];
      LowOp Call = Exit.Ops[4];
      if (!Checker) {
        Call.Inputs[0] = NdVar::cst(0x6000, 4);
        auto &Callee = F.Callees.emplace_back();
        Callee.Target = 0x6000;
      }
      LowBlock Sequence;
      const auto Raise = Deallocated ? 36u : 16u;
      // With ESP initially at -56, raising it by 36 reuses the second
      // cookie at -24 for the return PC. The following restoration hides
      // this from a check made only at block entry or block exit.
      emitOp(Sequence, 0x1030, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Raise, 4)});
      Sequence.Ops.push_back(Call);
      emitOp(Sequence, 0x1030, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Raise, 4)});
      if (Checker) {
        Exit.Ops.erase(Exit.Ops.begin() + 4);
        Exit.Ops.insert(Exit.Ops.begin() + 4, Sequence.Ops.begin(),
                        Sequence.Ops.end());
      } else {
        // Decode ECX again after the ordinary call, then execute the original
        // checked exit. Both variants therefore cover the same cookie read.
        for (auto &Op : Exit.Ops)
          Op.Addr += 5;
        for (auto &Boundary : Exit.InstructionBoundaries)
          Boundary.Address += 5;
        auto Boundary = Exit.InstructionBoundaries.front();
        Boundary.Address = 0x1030;
        Exit.InstructionBoundaries.insert(Exit.InstructionBoundaries.begin(),
                                          Boundary);
        Exit.EndAddr += 5;
        Exit.Ops.insert(Exit.Ops.begin(), Sequence.Ops.begin(),
                        Sequence.Ops.end());
        auto &Return = F.Function.Blocks[4];
        Return.StartAddr += 16;
        Return.EndAddr += 16;
        for (auto &Op : Return.Ops)
          Op.Addr += 16;
        for (auto &Boundary : Return.InstructionBoundaries)
          Boundary.Address += 16;
      }
      for (size_t I = 0; I != Exit.Ops.size(); ++I)
        Exit.Ops[I].Seq = I;
      const auto Result = F.analyze();
      EXPECT_EQ(Result.SecurityCookiesComplete, !Deallocated);
    }
}
} // namespace

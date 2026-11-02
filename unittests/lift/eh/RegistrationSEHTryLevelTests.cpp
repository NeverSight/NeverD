//===- RegistrationSEHTryLevelTests.cpp - Callback state reload -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks authenticated state writes and correlated runtime unwind outcomes.

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

namespace {

void addReturn(LowBlock &Block, va_t Address) {
  const size_t First = Block.Ops.size();
  emitOp(Block, Address, NdOp::RETURN, {}, {});
  Block.EndAddr = Address + 1;
  Block.InstructionBoundaries.push_back({Address, 1, First, 1});
  Block.InstructionBoundaries.back().Control = LowInstructionControl::Return;
  Block.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Return;
}

void addMarker(LowBlock &Block, va_t Address, int32_t Value) {
  const size_t First = Block.Ops.size();
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(1, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Value), 4)});
  emitOp(Block, Address, NdOp::STORE, {}, {NdVar::tmp(0, 4), NdVar::tmp(1, 4)});
  Block.InstructionBoundaries.push_back({Address, 6, First, 3});
}

void addState(LowFunc &F, unsigned Id, va_t Address, int32_t Value,
              uint16_t Width = 4) {
  auto &Block = F.Blocks[Id];
  const size_t First = Block.Ops.size();
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-4), 4)});
  emitOp(Block, Address, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::cst(uint32_t(Value), Width)});
  Block.EndAddr = Address + 7;
  Block.InstructionBoundaries.push_back({Address, 7, First, 2});
  F.ExceptionMetadata->Registration->TryLevelStores.push_back(
      {Address, Address + 7, Value, uint8_t(Width)});
}

LowFunc stateUnwindFrame(int32_t Written = -1, uint16_t Width = 4,
                         uint32_t Bound = UINT32_MAX, bool Branch = false) {
  auto F = makeBranchingFrame();
  F.Blocks.resize(13);
  for (unsigned I = 1; I != F.Blocks.size(); ++I) {
    F.Blocks[I] = {};
    F.Blocks[I].Id = I;
  }
  F.Blocks[0].InstructionBoundaries.front().OpCount = F.Blocks[0].Ops.size();
  F.Blocks[0].Succs = {1};
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes = {
      {-1, 0, 0x1a00, true}, {0, 0, 0x1900, true}, {1, 0, 0x1800, true}};
  Chain.TryLevelStores.clear();
  auto &Setup = F.Blocks[1];
  Setup.StartAddr = 0x1080;
  Setup.EndAddr = 0x108c;
  Setup.Succs = {2};
  emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(160, 4)});
  emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1083, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::reg(x86reg::RSP, 4)});
  Setup.InstructionBoundaries = {{0x1080, 3, 0, 1}, {0x1083, 3, 1, 2}};
  addMarker(Setup, 0x1086, -40);
  F.Blocks[2].StartAddr = 0x1100;
  F.Blocks[2].Succs = {3};
  addState(F, 2, 0x1100, 2);
  auto &Call = F.Blocks[3];
  Call.StartAddr = 0x1110;
  Call.EndAddr = 0x111b;
  Call.Succs = {4};
  emitOp(Call, 0x1110, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  emitOp(Call, 0x1110, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Bound, 4)});
  emitOp(Call, 0x1112, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Call, 0x1115, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  emitOp(Call, 0x1115, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::reg(x86reg::RDX, 4)});
  emitOp(Call, 0x1116, NdOp::CALL, {}, {NdVar::cst(0x2100, 4)});
  Call.InstructionBoundaries = {{0x1110, 2, 0, 2},
                                {0x1112, 3, 2, 1},
                                {0x1115, 1, 3, 2},
                                {0x1116, 5, 5, 1}};
  Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  auto &Observe = F.Blocks[4];
  Observe.StartAddr = 0x111b;
  Observe.EndAddr = 0x1121;
  Observe.Succs = {5};
  emitOp(Observe, 0x111b, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
  emitOp(Observe, 0x111e, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
         {NdVar::tmp(0, 4)});
  // Instruction-local temporaries do not survive the address change.
  Observe.Ops.back().Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  Observe.Ops.front().Output = NdVar::reg(x86reg::RCX, 4);
  Observe.InstructionBoundaries = {{0x111b, 3, 0, 1}, {0x111e, 3, 1, 1}};
  F.Blocks[5].StartAddr = 0x1130;
  F.Blocks[5].Succs = {6};
  addState(F, 5, 0x1130, -1);
  auto &Unlink = F.Blocks[6];
  Unlink.StartAddr = 0x1140;
  Unlink.EndAddr = 0x1144;
  emitOp(Unlink, 0x1140, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Unlink, 0x1140, NdOp::LOAD, NdVar::tmp(1, 4), {NdVar::tmp(0, 4)});
  emitOp(Unlink, 0x1140, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(1, 4)},
         NdMemoryAddressSpace::X86FS);
  Unlink.InstructionBoundaries = {{0x1140, 4, 0, 3}};
  auto &Outer = F.Blocks[7];
  Outer.StartAddr = 0x1800;
  Outer.EndAddr = 0x1802;
  Outer.Succs = Branch ? std::vector<int>{8, 10} : std::vector<int>{8};
  emitOp(Outer, 0x1800, Branch ? NdOp::COND_BR : NdOp::BRANCH, {},
         {NdVar::cst(0x1810, 8)});
  if (Branch)
    Outer.Ops.back().addInput(NdVar::reg(x86reg::ZF, 1));
  Outer.InstructionBoundaries = {{0x1800, 2, 0, 1}};
  Outer.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Outer.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch;
  if (Branch)
    Outer.InstructionBoundaries.back().ControlFlags |=
        LowInstructionControlFlag::Conditional;
  auto &First = F.Blocks[8];
  First.StartAddr = 0x1810;
  First.Succs = {9};
  if (Branch)
    addMarker(First, 0x1810, 0);
  addState(F, 8, Branch ? 0x1816 : 0x1810, Written, Width);
  auto &Return = F.Blocks[9];
  Return.StartAddr = 0x1840;
  addReturn(Return, 0x1840);
  auto &Other = F.Blocks[10];
  Other.StartAddr = 0x1830;
  Other.Succs = {9};
  addMarker(Other, 0x1830, -40);
  addState(F, 10, 0x1836, 1);
  auto &Middle = F.Blocks[11];
  Middle.StartAddr = 0x1900;
  addMarker(Middle, 0x1900, 0);
  addReturn(Middle, 0x1906);
  auto &Inner = F.Blocks[12];
  Inner.StartAddr = 0x1a00;
  addMarker(Inner, 0x1a00, -44);
  addReturn(Inner, 0x1a06);
  return F;
}

RegistrationStateAnalysis analyze(const LowFunc &F) {
  const std::vector<RegistrationLocalUnwindContract> Calls{{0x2100, false}};
  return analyzeRegistrationStates(F, 0, 0, nullptr, nullptr, nullptr, &Calls);
}

void expectMarker(const RegistrationStateAnalysis &A, int32_t Offset) {
  const auto Found = std::find_if(
      A.FrameValues.begin(), A.FrameValues.end(), [](const auto &Value) {
        return Value.Address == 0x111e && Value.OpSeq == 1;
      });
  ASSERT_NE(Found, A.FrameValues.end());
  EXPECT_EQ(Found->EstablishedFrameOffset, Offset);
}

} // namespace

TEST(RegistrationSEHTryLevel, RetirementSkipsRemainingFinallyEffects) {
  const auto A = analyze(stateUnwindFrame());
  for (const auto &Diagnostic : A.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(A.Complete);
  ASSERT_TRUE(A.SEHStateStoresComplete);
  ASSERT_EQ(A.Blocks[4].levels(), (std::vector<int32_t>{-1}));
  expectMarker(A, -40);
  const auto Store =
      std::find_if(A.SEHStateStores.begin(), A.SEHStateStores.end(),
                   [](const auto &S) { return S.Address == 0x1810; });
  ASSERT_NE(Store, A.SEHStateStores.end());
  EXPECT_EQ(Store->Transitions,
            (std::vector<std::pair<int32_t, int32_t>>{{1, -1}}));
}

TEST(RegistrationSEHTryLevel, BranchingCallbackKeepsStateAndFrameCorrelated) {
  const auto A = analyze(stateUnwindFrame(0, 4, 0, true));
  for (const auto &Diagnostic : A.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(A.Complete);
  ASSERT_TRUE(A.SEHStateStoresComplete);
  EXPECT_EQ(A.Blocks[4].levels(), (std::vector<int32_t>{0}));
  // The zero-state arm writes the same pointer as the one-state arm's
  // subsequent cleanup. Joining arms before choosing the cleanup loses it.
  expectMarker(A, 0);
}

TEST(RegistrationSEHTryLevel, DeepUnwindKeepsTheDefaultProofBudget) {
  for (bool ReverseBlocks : {false, true}) {
    SCOPED_TRACE(ReverseBlocks);
    auto F = stateUnwindFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    // Search-only rows still advance the local-unwind cursor. The one
    // traversed finally must retain its write after the whole deep walk.
    constexpr int32_t Count = 1024;
    Chain.Scopes.resize(Count);
    for (int32_t I = 2; I != Count; ++I)
      Chain.Scopes[I] = {I - 1, 0x1b00, 0x1c00, false};
    Chain.TryLevelStores.front().Level = Count - 1;
    F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(Count - 1, 4);
    if (ReverseBlocks)
      std::reverse(F.Blocks.begin(), F.Blocks.end());
    const auto A = analyze(F);
    for (const auto &Diagnostic : A.Diagnostics)
      ADD_FAILURE() << Diagnostic;
    ASSERT_TRUE(A.Complete);
    EXPECT_TRUE(A.RegistrationLifetimeComplete);
    const auto Resume =
        std::find_if(A.Blocks.begin(), A.Blocks.end(),
                     [](const auto &B) { return B.BlockId == 4; });
    ASSERT_NE(Resume, A.Blocks.end());
    EXPECT_EQ(Resume->levels(), (std::vector<int32_t>{-1}));
    expectMarker(A, -44);
  }
}

TEST(RegistrationSEHTryLevel, ConvergingUnwindBranchesHaveSeparateAncestors) {
  const auto A = analyze(stateUnwindFrame(0, 4, UINT32_MAX, true));
  for (const auto &Diagnostic : A.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(A.Complete);
  EXPECT_EQ(A.Blocks[4].levels(), (std::vector<int32_t>{-1}));
  // Both arms reach the same outer frame. A completed sibling traversal is
  // not a recurrent configuration on the current path.
  expectMarker(A, -44);
}

TEST(RegistrationSEHTryLevel,
     RecurrentUnwindFrameCannotBorrowATerminalSibling) {
  for (bool Branch : {false, true}) {
    SCOPED_TRACE(Branch);
    const auto A = analyze(stateUnwindFrame(2, 4, 0, Branch));
    EXPECT_FALSE(A.Complete);
    EXPECT_FALSE(A.RegistrationLifetimeComplete);
  }
}

TEST(RegistrationSEHTryLevel, NarrowWritesPreserveTheReachingHighBytes) {
  for (uint16_t Width : {1, 2}) {
    SCOPED_TRACE(Width);
    auto F = stateUnwindFrame(1, Width, 0);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes.resize(259, {-1, 0, 0x1900, true});
    Chain.Scopes[258] = {256, 0, 0x1800, true};
    Chain.Scopes[257] = {0, 0, 0x1900, true};
    Chain.TryLevelStores.front().Level = 258;
    F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(258, 4);
    const auto A = analyze(F);
    ASSERT_TRUE(A.Complete);
    ASSERT_TRUE(A.SEHStateStoresComplete);
    const auto Store =
        std::find_if(A.SEHStateStores.begin(), A.SEHStateStores.end(),
                     [](const auto &S) { return S.Address == 0x1810; });
    ASSERT_NE(Store, A.SEHStateStores.end());
    const int32_t Written = Width == 1 ? 257 : 1;
    EXPECT_NE(std::find(Store->Transitions.begin(), Store->Transitions.end(),
                        std::pair<int32_t, int32_t>{256, Written}),
              Store->Transitions.end());
    expectMarker(A, 0);
  }
}

TEST(RegistrationSEHTryLevel, CallbackStateStoreNeedsItsExactSourceRecipe) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = stateUnwindFrame();
    auto &Block = F.Blocks[8];
    auto &Store = Block.Ops.back();
    auto &Observed = F.ExceptionMetadata->Registration->TryLevelStores[2];
    switch (Mutation) {
    case 0: // Re-activate the currently unwinding scope indefinitely.
      Store.Inputs[1] = NdVar::cst(2, 4);
      Observed.Level = 2;
      break;
    case 1:
      Block.Ops.front().Inputs[1] = NdVar::cst(uint32_t(-3), 4);
      break;
    case 2:
      Store.Inputs[1].Size = 2;
      break;
    case 3:
      Store.Inputs[1] = NdVar::cst(0, 4);
      break;
    case 4:
      --Observed.EndVA;
      break;
    case 5:
      Store.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 6:
      Store.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 7:
      Store.Opcode = NdOp::COPY;
      Store.Output = NdVar::reg(x86reg::RAX, 4);
      Store.NumInputs = 1;
      break;
    case 8:
      Store.Seq = -1;
      break;
    case 9:
      Block.Ops.front().Inputs[1] = NdVar::cst(uint32_t(-24), 4);
      break;
    case 11:
      ++Block.InstructionBoundaries.back().Size;
      break;
    case 10: {
      // One reachable return does not prove that a recurrent alternative
      // returns. The old fixed-point join silently discarded this arm.
      F.Blocks.resize(14);
      auto &Loop = F.Blocks[9];
      Loop.Ops.clear();
      emitOp(Loop, 0x1840, NdOp::COND_BR, {},
             {NdVar::cst(0x1840, 8), NdVar::reg(x86reg::ZF, 1)});
      Loop.EndAddr = 0x1842;
      Loop.Succs = {9, 13};
      Loop.InstructionBoundaries = {{0x1840, 2, 0, 1}};
      Loop.InstructionBoundaries.front().Control =
          LowInstructionControl::Branch;
      Loop.InstructionBoundaries.front().ControlFlags =
          LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional;
      F.Blocks[13].Id = 13;
      F.Blocks[13].StartAddr = 0x1842;
      addReturn(F.Blocks[13], 0x1842);
      break;
    }
    }
    const auto A = analyze(F);
    EXPECT_FALSE(A.Complete);
    EXPECT_FALSE(A.SEHStateStoresComplete);
    EXPECT_TRUE(A.SEHStateStores.empty());
  }
}

//===- RegistrationCatchContextTests.cpp - Live PE32 catch objects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep distinct reference objects alive across nested catch invocations.
//===----------------------------------------------------------------------===//
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

void readObject(LowBlock &Block, va_t Address, int32_t Home, uint32_t Offset) {
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(60, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Home), 4)});
  emitOp(Block, Address, NdOp::LOAD, NdVar::tmp(61, 4), {NdVar::tmp(60, 4)});
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(62, 4),
         {NdVar::tmp(61, 4), NdVar::cst(Offset, 4)});
  emitOp(Block, Address, NdOp::LOAD, NdVar::tmp(63, 4), {NdVar::tmp(62, 4)});
}

LowFunc makeNestedReferenceCatches(unsigned Mutation = 0) {
  auto F = makeCxxCatchContinuation();
  const auto OriginalReturn = F.Blocks[5];
  auto &EH = *F.ExceptionMetadata;
  auto Outer = EH.Cxx->TryBlocks[0];
  Outer.CatchHigh = 3;
  Outer.Handlers[0].TypeDescriptorVA = 0x3000;
  Outer.Handlers[0].CatchObjectOffset = -24;
  Outer.Handlers[0].Adjectives = 8;
  auto Inner = Outer;
  Inner.TryLow = Inner.TryHigh = 2;
  Inner.Handlers[0].HandlerVA = 0x1850;
  Inner.Handlers[0].TypeDescriptorVA = 0x3100;
  Inner.Handlers[0].CatchObjectOffset = -20;
  EH.Cxx->MaxState = 4;
  EH.Cxx->UnwindMap = {{-1, 0}, {-1, 0}, {1, 0}, {1, 0}};
  EH.Cxx->TryBlocks = {Inner, Outer};
  EH.Registration->TryLevelStores.push_back({0x1800, 0x1807, 2});
  F.Blocks.resize(10);
  auto &Enter = F.Blocks[5];
  Enter.Ops.clear();
  Enter.InstructionBoundaries = {{0x1800, 7}};
  Enter.EndAddr = 0x1807;
  Enter.Succs = {9};
  addSlotStore(Enter, 2);
  auto &Protected = F.Blocks[9];
  Protected.Id = 9;
  Protected.StartAddr = 0x1810;
  Protected.EndAddr = 0x1811;
  Protected.InstructionBoundaries = {{0x1810, 1}};
  Protected.Succs = {8};
  auto ConfigureReturn = [&](unsigned Index, va_t Begin, va_t Target) {
    auto &Block = F.Blocks[Index];
    Block = OriginalReturn;
    Block.Id = Index;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 6;
    for (auto &Boundary : Block.InstructionBoundaries)
      Boundary.Address += Begin - OriginalReturn.StartAddr;
    Block.Ops.clear();
    auto AddReturn = [&] {
      for (auto Op : OriginalReturn.Ops) {
        Op.Addr += Begin - OriginalReturn.StartAddr;
        Op.Seq = Block.Ops.size();
        if (Op.Opcode == NdOp::COPY)
          Op.Inputs[0] = NdVar::cst(Target, 4);
        Block.Ops.push_back(Op);
      }
    };
    if (Index == 7) {
      readObject(Block, Begin, -24, Mutation == 1 ? 4 : 0);
      readObject(Block, Begin, -20, Mutation == 2 ? 8 : 4);
    } else {
      readObject(Block, Begin, Mutation == 3 ? -20 : -24, 0);
    }
    AddReturn();
  };
  ConfigureReturn(7, 0x1850, 0x1860);
  ConfigureReturn(8, 0x1860, 0x1900);
  // A normal exit from the protected body also resumes the same outer catch.
  // Keep its state equal to the inner catch's exit so the test isolates object
  // identity rather than try-level joins.
  addSlotStore(Protected, 3);
  Protected.EndAddr = 0x1817;
  Protected.InstructionBoundaries = {{0x1810, 7}};
  EH.Registration->TryLevelStores.push_back({0x1810, 0x1817, 3});
  return F;
}

std::vector<RegistrationCalleeFrameContract> throws() {
  std::vector<RegistrationCalleeFrameContract> Result(2);
  for (unsigned I = 0; I != 2; ++I) {
    auto &C = Result[I];
    C.CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
    C.Target = 0x2100 + I * 0x100;
    C.DoesNotReturn = true;
    C.ThrownTypeVA = 0x3000 + I * 0x100;
    C.ThrownObjectSize = I == 0 ? 4 : 8;
  }
  return Result;
}

TEST(RegistrationCatchContext, KeepsBothObjectsAndRestoresTheOuterLifetime) {
  const auto Calls = throws();
  const auto Result =
      analyzeRegistrationStates(makeNestedReferenceCatches(), 0, 0, &Calls);
  ASSERT_TRUE(Result.Complete)
      << (Result.Diagnostics.empty() ? "" : Result.Diagnostics.front());
  ASSERT_TRUE(Result.CxxContinuationsComplete);
  ASSERT_TRUE(Result.RuntimeObjectAccessesComplete);
  ASSERT_EQ(Result.RuntimeObjectAccesses.size(), 3u);
  EXPECT_EQ(Result.RuntimeObjectAccesses[0].TryIndex, 1u);
  EXPECT_EQ(Result.RuntimeObjectAccesses[0].Offset, 0);
  EXPECT_EQ(Result.RuntimeObjectAccesses[1].TryIndex, 0u);
  EXPECT_EQ(Result.RuntimeObjectAccesses[1].Offset, 4);
  EXPECT_EQ(Result.RuntimeObjectAccesses[2].TryIndex, 1u);
  EXPECT_EQ(Result.RuntimeObjectAccesses[2].Offset, 0);
}

TEST(RegistrationCatchContext, RejectsWrongObjectBoundsAndExitedLifetimes) {
  const auto Calls = throws();
  for (unsigned Mutation = 1; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Result = analyzeRegistrationStates(
        makeNestedReferenceCatches(Mutation), 0, 0, &Calls);
    EXPECT_FALSE(Result.RuntimeObjectAccessesComplete);
    EXPECT_TRUE(Result.RuntimeObjectAccesses.empty());
  }
}
} // namespace

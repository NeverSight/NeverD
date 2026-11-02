//===- RegistrationInitializerStateTests.cpp - PE32 ctor flow -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check argument privacy, callee cleanup and normal-edge initialization.
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc constructorFlow(unsigned Mutation) {
  auto F = makeCxxCatchContinuation();
  auto &Body = F.Blocks[1];
  Body.Succs = {7};
  emitOp(Body, 0x1013, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
  emitOp(Body, 0x1013, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4),
          Mutation == 2 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(7, 4)});
  emitOp(Body, 0x1013, NdOp::INT_ADD, NdVar::tmp(60, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  if (Mutation != 1)
    emitOp(Body, 0x1013, NdOp::STORE, {},
           {NdVar::tmp(60, 4), NdVar::cst(11, 4)});
  emitOp(Body, 0x1013, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 3 ? -32 : -24), 4)});
  auto Call = [&](int Id, va_t Address, va_t Target, int Next) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = Address;
    B.EndAddr = Address + 5;
    B.Succs = {Next};
    LowInstructionBoundary Boundary;
    Boundary.Address = Address;
    Boundary.Size = 5;
    Boundary.Control = LowInstructionControl::Call;
    B.InstructionBoundaries.push_back(Boundary);
    emitOp(B, Address, NdOp::CALL, NdVar::tmp(61, 4), {NdVar::cst(Target, 4)});
    return B;
  };
  auto Init = Call(7, 0x1100, 0x2100, 8);
  emitOp(Init, 0x1100, NdOp::COPY, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(61, 4)});
  F.Blocks.push_back(std::move(Init));
  F.Blocks.push_back(Call(8, 0x1110, 0x2200, 2));
  if (Mutation == 5) {
    auto &Catch = F.Blocks[5];
    LowBlock Read = Call(9, 0x1800, 0x2200, 5);
    Read.Ops.clear();
    emitOp(Read, 0x1800, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Read, 0x1800, NdOp::CALL, {}, {NdVar::cst(0x2200, 4)});
    Catch.StartAddr = 0x1805;
    Catch.InstructionBoundaries.front().Address = 0x1805;
    Catch.InstructionBoundaries.back().Address = 0x180a;
    Catch.EndAddr = 0x180b;
    for (auto &Op : Catch.Ops)
      Op.Addr += 5;
    F.Blocks.push_back(std::move(Read));
  }
  return F;
}

TEST(RegistrationInitializerState, PreservesArgumentsObjectAndNormalReturn) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto F = constructorFlow(Mutation);
    RegistrationCalleeFrameContract Init;
    Init.CalleeKind = RegistrationCalleeFrameContract::Kind::Initializer;
    Init.Target = 0x2100;
    Init.StackPopBytes = Mutation == 6 ? 2 : 8;
    Init.ECXReads = Init.ECXWrites = {{0, 8}};
    Init.ECXInitialized = {{0, Mutation == 4 ? 4 : 8}};
    RegistrationCalleeFrameContract Read;
    Read.Target = 0x2200;
    Read.ECXReads = {{0, 8}};
    const std::vector Contracts{Init, Read};
    const auto Result = analyzeRegistrationStates(F, 0, 0, &Contracts);
    EXPECT_EQ(Result.CallFrameEffectsComplete, Mutation == 0);
    if (!Mutation) {
      for (const auto &D : Result.Diagnostics)
        ADD_FAILURE() << D;
      ASSERT_EQ(Result.CallFrameEffects.size(), 2u);
      EXPECT_EQ(Result.CallFrameEffects[0].StackPopBytes, 8u);
      EXPECT_EQ(Result.CallFrameEffects[1].ECXFrameOffset, -24);
    }
  }
}
} // namespace

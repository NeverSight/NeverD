//===- RegistrationScalarCallStateTests.cpp - Scalar results -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace {
using namespace neverd;
using namespace neverd::registration_test;

LowFunc makeScalarCall(bool Catch, NdVar Result, NdVar Stored) {
  auto Function = makeCxxCatchContinuation();
  LowBlock Call;
  Call.Id = 7;
  Call.StartAddr = Catch ? 0x1800 : 0x1100;
  Call.EndAddr = Call.StartAddr + 10;
  Call.InstructionBoundaries = {{Call.StartAddr, 5}, {Call.StartAddr + 5, 5}};
  Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
  emitOp(Call, Call.StartAddr, NdOp::CALL, Result, {NdVar::cst(0x2100, 4)});
  // Keep the lifter's instruction-local result alive until its EAX projection.
  if (Result.isTemp())
    emitOp(Call, Call.StartAddr, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {Result});
  emitOp(Call, Call.StartAddr + 5, NdOp::STORE, {},
         {NdVar::cst(0x3000, 4), Stored});
  if (Catch) {
    auto &Handler = Function.Blocks[5];
    Handler.StartAddr = Call.EndAddr;
    for (auto &Op : Handler.Ops)
      Op.Addr += 10;
    for (auto &Boundary : Handler.InstructionBoundaries)
      Boundary.Address += 10;
    Handler.EndAddr += 10;
    Call.Succs = {5};
  } else {
    Function.Blocks[1].Succs = {7};
    Call.Succs = {2};
  }
  Function.Blocks.push_back(std::move(Call));
  return Function;
}

RegistrationCalleeFrameContract scalarContract(bool Independent = true) {
  RegistrationCalleeFrameContract Contract;
  Contract.Target = 0x2100;
  Contract.HasIndependentScalarReturn = Independent;
  return Contract;
}

TEST(RegistrationScalarCallState, ComposesOnlyTheProvedReturnInParentAndCatch) {
  for (bool Catch : {false, true})
    for (bool Temporary : {false, true}) {
      SCOPED_TRACE(Catch);
      SCOPED_TRACE(Temporary);
      const auto Result =
          Temporary ? NdVar::tmp(64, 4) : NdVar::reg(x86reg::RAX, 4);
      const auto Function =
          makeScalarCall(Catch, Result, NdVar::reg(x86reg::RAX, 4));
      const std::vector Contracts{scalarContract()};
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.CallFrameEffectsComplete);
      EXPECT_TRUE(States.RuntimeObjectAccessesComplete);
      ASSERT_EQ(States.CallFrameEffects.size(), 1u);
      EXPECT_EQ(States.CallFrameEffects.front().Address,
                Catch ? 0x1800u : 0x1100u);
    }
}

TEST(RegistrationScalarCallState, UnprovedAndWideReturnsRetainEscapeTaint) {
  for (bool Catch : {false, true})
    for (uint16_t Width : {2, 4, 8}) {
      SCOPED_TRACE(Catch);
      SCOPED_TRACE(Width);
      const auto Result = NdVar::tmp(64, Width);
      const auto Function =
          makeScalarCall(Catch, Result, NdVar::reg(x86reg::RAX, 4));
      const std::vector Contracts{scalarContract(Width != 4)};
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
      EXPECT_FALSE(States.Complete);
    }
}

TEST(RegistrationScalarCallState, OtherVolatileResultsRetainEscapeTaint) {
  for (bool Catch : {false, true})
    for (uint64_t Register : {x86reg::RCX, x86reg::RDX}) {
      SCOPED_TRACE(Catch);
      SCOPED_TRACE(Register);
      const auto Function =
          makeScalarCall(Catch, NdVar::tmp(64, 4), NdVar::reg(Register, 4));
      const std::vector Contracts{scalarContract()};
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
      EXPECT_FALSE(States.Complete);
    }
}

TEST(RegistrationScalarCallState, ChangedOutputsCannotGrantScalarAuthority) {
  for (bool Catch : {false, true})
    for (uint64_t Register : {x86reg::RCX, x86reg::RDX, x86reg::RAX + 1}) {
      SCOPED_TRACE(Catch);
      SCOPED_TRACE(Register);
      const auto Result = NdVar::reg(Register, 4);
      const auto Function = makeScalarCall(Catch, Result, Result);
      const std::vector Contracts{scalarContract()};
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
      EXPECT_FALSE(States.Complete);
    }
}

TEST(RegistrationScalarCallState, ScalarReturnsCannotInitializeBorrowedBytes) {
  for (bool Initialized : {false, true}) {
    SCOPED_TRACE(Initialized);
    auto Function =
        makeScalarCall(false, NdVar::tmp(64, 4), NdVar::reg(x86reg::RAX, 4));
    LowBlock Setup;
    emitOp(Setup, 0x1013, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    if (Initialized)
      emitOp(Setup, 0x1013, NdOp::STORE, {},
             {NdVar::reg(x86reg::RCX, 4), NdVar::cst(7, 4)});
    auto &Body = Function.Blocks[1];
    const auto StateStore =
        std::find_if(Body.Ops.begin(), Body.Ops.end(),
                     [](const auto &Op) { return Op.Addr == 0x1016; });
    Body.Ops.insert(StateStore, Setup.Ops.begin(), Setup.Ops.end());
    for (size_t I = 0; I != Body.Ops.size(); ++I)
      Body.Ops[I].Seq = I;

    auto Writer = scalarContract();
    Writer.ECXWrites = {{0, 4}};
    auto Reader = scalarContract();
    Reader.Target = 0x2200;
    Reader.ECXReads = {{0, 4}};
    LowBlock Read;
    Read.Id = 8;
    Read.StartAddr = 0x1110;
    Read.EndAddr = 0x1118;
    Read.InstructionBoundaries = {{0x1110, 3}, {0x1113, 5}};
    Read.InstructionBoundaries[1].Control = LowInstructionControl::Call;
    Read.Succs = {2};
    emitOp(Read, 0x1110, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Read, 0x1113, NdOp::CALL, NdVar::tmp(65, 4),
           {NdVar::cst(Reader.Target, 4)});
    Function.Blocks[7].Succs = {8};
    Function.Blocks.push_back(std::move(Read));
    const std::vector Contracts{Writer, Reader};
    const auto States = analyzeRegistrationStates(Function, 0, 0, &Contracts);
    EXPECT_EQ(States.Complete, Initialized);
    EXPECT_EQ(States.CallFrameEffectsComplete, Initialized);
    EXPECT_EQ(States.CallFrameEffects.size(), Initialized ? 2u : 1u);
  }
}
} // namespace

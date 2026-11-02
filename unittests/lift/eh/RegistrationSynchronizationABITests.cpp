//===- RegistrationSynchronizationABITests.cpp - Native runtime stack -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep SSA's outgoing stack cleanup consistent with the bound runtime ABI.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"

#include <cstdint>

namespace {
using namespace neverd;

TEST(RegistrationSynchronizationABI,
     ReturningStackUsesTheExactRuntimeSignature) {
  using Kind = RegistrationSEHBoundCall::Kind;
  for (auto Runtime : {Kind::GetStartupInfoW, Kind::HeapTermination,
                       Kind::InterlockedCompareExchange, Kind::Sleep})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Runtime) << ' ' << Mutation);
      LowFunc F;
      F.Entry = 0x401000;
      F.ExceptionMetadata.emplace().CodeRange = {F.Entry, F.Entry + 6};
      auto &States = F.RegistrationStates.emplace();
      States.Complete = States.CallbackStatesComplete = true;
      States.RegistrationLifetimeComplete = States.SEHBoundCallsComplete = true;
      States.HasBoundSEHCallees = true;
      auto &Block = F.Blocks.emplace_back();
      Block.StartAddr = F.Entry;
      Block.EndAddr = F.Entry + 6;
      auto &Call = Block.Ops.emplace_back();
      Call.Opcode = NdOp::INDIR_CALL;
      Call.Addr = F.Entry;
      Call.Seq = 0;
      Call.addInput(NdVar::cst(0x403000, 4));
      auto &Boundary = Block.InstructionBoundaries.emplace_back();
      Boundary.Address = F.Entry;
      Boundary.Size = 6;
      Boundary.OpCount = 1;
      Boundary.Control = LowInstructionControl::Call;
      auto &Receipt = States.SEHBoundCalls.emplace_back();
      Receipt.CalleeKind = Runtime;
      Receipt.Address = Call.Addr;
      Receipt.OpSeq = 0;
      Receipt.TargetVA = 0x403000;
      const uint32_t Words = Runtime == Kind::HeapTermination              ? 4
                             : Runtime == Kind::InterlockedCompareExchange ? 3
                                                                           : 1;
      Receipt.Arguments.resize(Words);
      if (Mutation == 1)
        Receipt.Arguments.pop_back();
      if (Mutation == 2)
        Receipt.Arguments.emplace_back();
      if (Mutation == 3)
        Receipt.CalleeKind = Kind::ControlFP;
      if (Mutation == 4)
        States.SEHBoundCallsComplete = false;
      if (Mutation == 5)
        ++Receipt.TargetVA;
      if (Mutation == 6)
        Boundary.Size = 5;
      if (Mutation == 7)
        Receipt.HasOrdinaryReturn = false;
      const auto Pop = registrationCallStackPop(F, Block, Call);
      if (Mutation == 0)
        EXPECT_EQ(Pop, Words * 4);
      else
        EXPECT_FALSE(Pop);
    }
}
} // namespace

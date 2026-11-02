//===- RegistrationMemberArgumentStateTests.cpp - Scalar source words -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace {
using namespace neverd;
using namespace neverd::registration_test;

enum class Receiver { Parent, ValueCatch, ReferenceCatch };

LowFunc memberWords(Receiver Kind, unsigned Words, unsigned Mutation = 0) {
  const bool Parent = Kind == Receiver::Parent;
  auto Function =
      Parent ? makeCxxCatchContinuation() : makeRealignedRegistrationFrame();
  if (!Parent) {
    auto &EH = *Function.ExceptionMetadata;
    EH.CodeRange.End = 0x2000;
    EH.Cxx->MaxState = 2;
    EH.Cxx->UnwindMap.resize(2, {-1, 0, CxxUnwindAction::ActionKind::None});
    CxxTryBlock Try;
    Try.TryLow = Try.TryHigh = 0;
    Try.CatchHigh = 1;
    auto &Catch = Try.Handlers.emplace_back();
    Catch.HandlerVA = 0x1800;
    Catch.TypeDescriptorVA = 0x3000;
    Catch.CatchObjectOffset = -36;
    Catch.Adjectives = Kind == Receiver::ReferenceCatch ? 8 : 0;
    EH.Cxx->TryBlocks = {Try};
  }
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto Local = NdVar::reg(x86reg::RSI, 4);
  const auto ECX = NdVar::reg(x86reg::RCX, 4);
  const auto Tmp = NdVar::reg(x86reg::RDX, 4);
  LowBlock Call;
  Call.Id = Parent ? 7 : 4;
  Call.StartAddr = Parent ? 0x1100 : 0x1800;
  auto Next = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs,
                  LowInstructionControl Control = LowInstructionControl::None) {
    const va_t Address = Call.StartAddr + Call.InstructionBoundaries.size() * 5;
    Call.InstructionBoundaries.push_back({Address, 5});
    Call.InstructionBoundaries.back().Control = Control;
    emitOp(Call, Address, Opcode, Output, Inputs);
  };
  if (Parent) {
    Next(NdOp::INT_ADD, ECX, {FP, NdVar::cst(uint32_t(-24), 4)});
    Next(NdOp::STORE, {}, {ECX, NdVar::cst(7, 4)});
  } else {
    Next(NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
    Next(NdOp::STORE, {}, {SP, FP});
    Next(NdOp::INT_ADD, Local, {FP, NdVar::cst(uint32_t(-60), 4)});
    Next(NdOp::INT_ADD, Tmp, {Local, NdVar::cst(40, 4)});
    Next(NdOp::LOAD, FP, {Tmp});
    Next(NdOp::INT_ADD, Tmp, {Local, NdVar::cst(44, 4)});
    Next(NdOp::STORE, {}, {Tmp, SP});
    Next(NdOp::INT_ADD, ECX, {Local, NdVar::cst(24, 4)});
    if (Kind == Receiver::ReferenceCatch)
      Next(NdOp::LOAD, ECX, {ECX});
  }
  Next(NdOp::INT_SUB, SP, {SP, NdVar::cst(Words * 4, 4)});
  for (unsigned I = 0; I != Words; ++I) {
    Next(NdOp::INT_ADD, Tmp, {SP, NdVar::cst(I * 4, 4)});
    if (Mutation == 1 && I == Words - 1)
      continue;
    const auto Value = Mutation == 3 ? SP
                       : Mutation == 4
                           ? ECX
                           : NdVar::cst(I + 3, Mutation == 2 ? 2 : 4);
    Next(NdOp::STORE, {}, {Tmp, Value});
  }
  Next(NdOp::CALL, NdVar::reg(x86reg::RAX, 4), {NdVar::cst(0x2200, 4)},
       LowInstructionControl::Call);
  if (Mutation == 7) {
    Next(NdOp::INT_ADD, Tmp, {SP, NdVar::cst(uint32_t(-4), 4)});
    Next(NdOp::LOAD, NdVar::reg(x86reg::RAX, 4), {Tmp});
    Next(NdOp::INT_ADD, ECX,
         {Parent ? FP : Local, NdVar::cst(Parent ? uint32_t(-24) : 24, 4)});
    if (Kind == Receiver::ReferenceCatch)
      Next(NdOp::LOAD, ECX, {ECX});
    // Reallocate the retired words without rewriting them. Neither parent
    // storage nor private callback storage can initialize this second call.
    Next(NdOp::INT_SUB, SP, {SP, NdVar::cst(Words * 4, 4)});
    Next(NdOp::CALL, NdVar::reg(x86reg::RAX, 4), {NdVar::cst(0x2200, 4)},
         LowInstructionControl::Call);
  }
  if (Parent) {
    Call.EndAddr = Call.StartAddr + Call.InstructionBoundaries.size() * 5;
    Call.Succs = {2};
    Function.Blocks[1].Succs = {7};
  } else {
    Next(NdOp::LOAD, FP, {SP});
    Next(NdOp::INT_ADD, SP, {SP, NdVar::cst(4, 4)});
    Next(NdOp::COPY, NdVar::reg(x86reg::RAX, 4), {NdVar::cst(0x1900, 4)});
    const va_t Address = Call.StartAddr + Call.InstructionBoundaries.size() * 5;
    Call.InstructionBoundaries.push_back({Address, 1});
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    emitOp(Call, Address, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});
    Call.EndAddr = Address + 1;
  }
  Function.Blocks.push_back(std::move(Call));
  if (!Parent) {
    LowBlock Resume;
    Resume.Id = 5;
    Resume.StartAddr = 0x1900;
    Resume.EndAddr = 0x1905;
    Resume.InstructionBoundaries = {{0x1900, 5}};
    Resume.Succs = {3};
    emitOp(Resume, 0x1900, NdOp::INT_ADD, Tmp,
           {FP, NdVar::cst(uint32_t(-16), 4)});
    emitOp(Resume, 0x1900, NdOp::LOAD, SP, {Tmp});
    emitOp(Resume, 0x1900, NdOp::INT_ADD, Local,
           {FP, NdVar::cst(uint32_t(-60), 4)});
    emitOp(Resume, 0x1900, NdOp::INT_ADD, Tmp, {Local, NdVar::cst(40, 4)});
    emitOp(Resume, 0x1900, NdOp::LOAD, FP, {Tmp});
    Function.Blocks.push_back(std::move(Resume));
  }
  for (auto &Block : Function.Blocks)
    for (size_t I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return Function;
}

std::vector<RegistrationCalleeFrameContract> contracts(unsigned Words,
                                                       unsigned Mutation = 0) {
  std::vector<RegistrationCalleeFrameContract> Result(2);
  Result[0].Target = 0x2100;
  Result[0].CalleeKind = RegistrationCalleeFrameContract::Kind::PrivateThrow;
  Result[0].DoesNotReturn = true;
  Result[0].ThrownTypeVA = 0x3000;
  Result[0].ThrownObjectSize = 12;
  Result[1].Target = 0x2200;
  Result[1].StackPopBytes =
      Mutation == 6 ? 2 : Words * 4 + (Mutation == 5 ? 4 : 0);
  Result[1].HasIndependentScalarReturn = true;
  Result[1].ECXReads = Result[1].ECXWrites = {{0, 4}};
  return Result;
}

TEST(RegistrationMemberArgumentState, BindsParentAndCatchWordsSeparately) {
  for (auto Kind :
       {Receiver::Parent, Receiver::ValueCatch, Receiver::ReferenceCatch})
    for (unsigned Words : {1u, 2u, 3u}) {
      auto Function = memberWords(Kind, Words);
      const auto Callees = contracts(Words);
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Callees);
      std::string Diagnostics;
      for (const auto &Diagnostic : States.Diagnostics)
        Diagnostics += Diagnostic + "\n";
      for (const auto &Block : States.Blocks)
        if (Block.Unknown)
          Diagnostics +=
              "unknown block " + std::to_string(Block.BlockId) + "\n";
      Diagnostics +=
          "calls=" + std::to_string(States.CallFrameEffects.size()) +
          " returns=" + std::to_string(States.CxxContinuations.size());
      ASSERT_TRUE(States.Complete) << unsigned(Kind) << ":" << Words << "\n"
                                   << Diagnostics;
      ASSERT_EQ(States.CallFrameEffects.size(), 1u);
      EXPECT_EQ(States.CallFrameEffects.front().StackPopBytes, Words * 4);
      EXPECT_EQ(States.CallFrameEffects.front().ECXRuntimeObject.has_value(),
                Kind == Receiver::ReferenceCatch);
      std::reverse(Function.Blocks.begin(), Function.Blocks.end());
      const auto Reordered =
          analyzeRegistrationStates(Function, 0, 0, &Callees);
      EXPECT_TRUE(Reordered.Complete);
      EXPECT_EQ(Reordered.CallFrameEffects, States.CallFrameEffects);
    }
}

TEST(RegistrationMemberArgumentState, RejectsMissingPointerAndUnbalancedWords) {
  for (auto Kind :
       {Receiver::Parent, Receiver::ValueCatch, Receiver::ReferenceCatch})
    for (unsigned Mutation = 1; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SCOPED_TRACE(unsigned(Kind));
      const auto Function = memberWords(Kind, 2, Mutation);
      const auto Callees = contracts(2, Mutation);
      const auto States = analyzeRegistrationStates(Function, 0, 0, &Callees);
      EXPECT_FALSE(States.Complete);
      EXPECT_FALSE(States.CallFrameEffectsComplete);
    }
}

TEST(RegistrationMemberArgumentState, RetiredWordsCannotInitializeAnotherCall) {
  for (auto Kind :
       {Receiver::Parent, Receiver::ValueCatch, Receiver::ReferenceCatch}) {
    SCOPED_TRACE(unsigned(Kind));
    const auto Function = memberWords(Kind, 2, 7);
    const auto Callees = contracts(2);
    const auto States = analyzeRegistrationStates(Function, 0, 0, &Callees);
    EXPECT_FALSE(States.Complete);
  }
}
} // namespace

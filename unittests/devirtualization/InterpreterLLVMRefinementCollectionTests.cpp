//===- InterpreterLLVMRefinementCollectionTests.cpp ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

#include <algorithm>
#include <set>

namespace neverd::analysis::llvm_refinement_test {
namespace {
using Collection = InterpreterLLVMNativeCollection;

InterpreterLLVMRefinementPreservation preserved() {
  return {{{x86reg::RBX, 8}},
          LowIRNativePreservationRequirement{
              LowPreservedStateSet::LegacyIntegerOpaqueV1,
              LowIRNativePreservationQuantifier::SelectedWitness}};
}

// CMP of identical operands sets ZF/PF and clears CF/AF/SF/OF. All other
// packed bits retain their canonical entry values. MOV EAX zero-extends.
std::string constantResult() {
  return module(R"(
    %flags = getelementptr i8, ptr %state, i64 128
    %old = load i64, ptr %flags, align 8
    %kept = and i64 %old, -2262
    %next = or i64 %kept, 68
    store i64 %next, ptr %flags, align 8
    store i64 7, ptr %state, align 8
    ret i64 0
  )");
}

void finiteCollection(bool Deferred) {
  // CMP EAX,EAX; JE suffix; dead RCL EDX,1; MOV EAX,7; RET.
  Program P({0x39, 0xc0, 0x74, 2, 0xd1, 0xd2, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  if (Deferred) {
    P.Image.Segments[0].Data[4] = 0x16;
    P.Image.Segments[0].Data[5] = 0x06;
  }
  const auto Required = preserved();
  const auto IR = constantResult();
  const auto Check = [&](const Collection &Policy, llvm::StringRef Source) {
    return P.check(R.Residual, Source, {}, {}, "model", Required, Policy);
  };
  const auto Strict = Check({}, IR);
  rejected(Strict, Stage::Native);
  EXPECT_EQ(Strict.Native.Proof.Status, Status::Unsupported);
  EXPECT_EQ(Strict.LLVM.Operations, 0U);
  if (Deferred)
    rejected(Check({true, false}, IR), Stage::Native);
  const Collection Policy{!Deferred, Deferred};
  const auto Good = Check(Policy, IR);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Native.Certificate->Relation.NativeAuditBoundaries.size(),
            Deferred ? 0U : 1U);
  EXPECT_TRUE(Good.Native.Certificate->Relation.NativePreservation);
  EXPECT_FALSE(Good.LLVM.Certificate->Contract.RetainUnauditedNativeBoundaries);
  EXPECT_FALSE(Good.LLVM.Certificate->Contract.DeferNativeConditionalEdges);
  auto BadIR = IR;
  BadIR.replace(BadIR.find("ret i64 0"), 9, "ret i64 1");
  const auto BadSource = Check(Policy, BadIR);
  rejected(BadSource, Stage::LLVM);
  EXPECT_TRUE(BadSource.Native.proved());
  EXPECT_EQ(BadSource.LLVM.Status, Status::Different);
  P.Image.Segments[0].Data[2] = 0x75; // JNE makes the bad arm feasible.
  const auto Reachable = Check(Policy, IR);
  rejected(Reachable, Stage::Native);
  EXPECT_EQ(Reachable.Native.Proof.Status, Status::Unsupported);
  EXPECT_EQ(Reachable.LLVM.Operations, 0U);
}
} // namespace

TEST(InterpreterLLVMRefinement, CollectionRetainsOnlyUnreachableBoundaries) {
  finiteCollection(false);
}

TEST(InterpreterLLVMRefinement, CollectionDefersOnlyInfeasibleEdges) {
  finiteCollection(true);
}

TEST(InterpreterLLVMRefinement, CollectionIsBoundOnlyInNativePremise) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module("ret i64 0");
  const auto Default = P.check(R.Residual, IR);
  ASSERT_TRUE(Default.proved()) << Default.Diagnostic;
  std::set<std::string> Native, Composite;
  for (bool Retained : {false, true})
    for (bool Deferred : {false, true}) {
      const auto Good =
          P.check(R.Residual, IR, {}, {}, "model", {}, {Retained, Deferred});
      ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
      const auto &N = Good.Native.Certificate->Relation.Contract;
      const auto &S = Good.LLVM.Certificate->Contract;
      EXPECT_EQ(N.RetainUnauditedNativeBoundaries, Retained);
      EXPECT_EQ(N.DeferNativeConditionalEdges, Deferred);
      EXPECT_FALSE(S.RetainUnauditedNativeBoundaries);
      EXPECT_FALSE(S.DeferNativeConditionalEdges);
      EXPECT_EQ(Good.LLVM.Certificate->InputDigest,
                Default.LLVM.Certificate->InputDigest);
      Native.insert(Good.Native.Certificate->InputDigest);
      Composite.insert(Good.Certificate->InputDigest);
      if (!Retained && !Deferred) {
        EXPECT_EQ(Good.Native.Certificate->InputDigest,
                  Default.Native.Certificate->InputDigest);
        EXPECT_EQ(Good.Certificate->InputDigest,
                  Default.Certificate->InputDigest);
      }
    }
  EXPECT_EQ(Native.size(), 4U);
  EXPECT_EQ(Composite.size(), 4U);
}

TEST(InterpreterLLVMRefinement, CollectionChecksBothInductivePremises) {
  for (bool Deferred : {false, true}) {
    SCOPED_TRACE(Deferred);
    // JRCXZ done; CMP EAX,EAX; JE body; dead RCL EDX,1;
    // body: LEA RCX,[RCX-1]; JMP entry; done: RET.
    Program P({0xe3, 12, 0x39, 0xc0, 0x74, 2, 0xd1, 0xd2, 0x48, 0x8d, 0x49,
               0xff, 0xeb, 0xf2, 0xc3});
    const auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    if (Deferred) {
      P.Image.Segments[0].Data[6] = 0x16;
      P.Image.Segments[0].Data[7] = 0x06;
    }
    const auto IR = module(R"(
    entry:
      %rcx = getelementptr i8, ptr %state, i64 8
      %flags = getelementptr i8, ptr %state, i64 128
      %initial = load i64, ptr %rcx, align 8
      %empty = icmp eq i64 %initial, 0
      br i1 %empty, label %exit, label %body
    body:
      %count = phi i64 [%initial, %entry], [%next, %body]
      %old = load i64, ptr %flags, align 8
      %kept = and i64 %old, -2262
      %updated = or i64 %kept, 68
      store i64 %updated, ptr %flags, align 8
      %next = sub i64 %count, 1
      %done = icmp eq i64 %next, 0
      br i1 %done, label %exit, label %body
    exit:
      store i64 0, ptr %rcx, align 8
      ret i64 0
    )");
    const auto Required = preserved();
    const Collection Policy{!Deferred, Deferred};
    InterpreterLLVMRefinementLimits Limited;
    Limited.NativeProof.Execution.MaxBlockVisits = 32;
    const auto Finite =
        P.check(R.Residual, IR, Limited, {}, "model", Required, Policy);
    rejected(Finite, Stage::Native);
    EXPECT_EQ(Finite.Native.Proof.Status, Status::BudgetExceeded);

    InterpreterLLVMRefinementPlans Plans;
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Entry + 2;
    Cut.UseEntryPrefix = true;
    unsigned Matches = 0;
    for (const auto &O : R.Origins)
      if (O.NativeInstruction.Address == Cut.OriginalAddress) {
        Cut.CandidateAddress = O.ResidualAddress;
        ++Matches;
      }
    ASSERT_EQ(Matches, 1U);
    const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
      const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
      const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
      Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
      Cut.OriginalState.push_back({L, T});
      Cut.CandidateState.push_back({L, T});
      return T;
    };
    const auto Count = Parameter(x86reg::RCX, 8);
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF})
      Parameter(Flag, 1);
    LowOp Nonzero;
    Nonzero.Opcode = NdOp::INT_NOTEQUAL;
    Nonzero.Output = NdVar::tmp(Cut.Inputs.size() * 8, 1);
    Nonzero.addInput(Count);
    Nonzero.addInput(NdVar::scalar(0, 8));
    Cut.Expressions = {Nonzero};
    Cut.Predicate = Nonzero.Output;
    Cut.Rank = {Count};
    Plans.Native = LowIRLoopRefinementPlan{{Cut}};
    auto Models = prepareInterpreterLLVMRefinement(R.Residual, IR, "model",
                                                   P.Frame, {}, Required);
    ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
    const auto Left = inferLowIRLoopRefinementPlan(Models->Residual.Function,
                                                   Models->Contract, {},
                                                   {Cut.CandidateAddress});
    va_t Body = 0;
    Matches = 0;
    for (const auto &B : Models->LLVM.Function.Blocks)
      if (std::any_of(B.Ops.begin(), B.Ops.end(), [](const auto &O) {
            return O.Opcode == NdOp::INT_SUB;
          })) {
        Body = B.StartAddr;
        ++Matches;
      }
    ASSERT_EQ(Matches, 1U);
    const auto Right = inferLowIRLoopRefinementPlan(
        Models->LLVM.Function, Models->Contract, {}, {Body});
    ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
    ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
    const auto &A = Left.Plan->Cutpoints.front();
    const auto &B = Right.Plan->Cutpoints.front();
    ASSERT_EQ(A.Rank.size(), 1U);
    ASSERT_EQ(B.Rank.size(), 1U);
    LowIRLoopCutpointPair Pair{A.OriginalAddress, B.OriginalAddress, {}};
    for (const auto &I : A.Inputs)
      for (const auto &J : B.Inputs)
        if (I.Side == LowIRLoopSide::Original &&
            I.Temporary == A.Rank.front() &&
            J.Side == LowIRLoopSide::Original && J.Temporary == B.Rank.front())
          Pair.SharedInputs.push_back({I.Location, J.Location});
    ASSERT_EQ(Pair.SharedInputs.size(), 1U);
    auto Paired = pairLowIRLoopRefinementPlans(*Left.Plan, *Right.Plan, {Pair});
    ASSERT_TRUE(bool(Paired)) << llvm::toString(Paired.takeError());
    Plans.LLVM = std::move(*Paired);
    const auto Good =
        P.check(R.Residual, IR, {}, Plans, "model", Required, Policy);
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    EXPECT_GT(Good.Native.Proof.LoopTransitions, 0U);
    EXPECT_GT(Good.Native.Proof.RankingChecks, 0U);
    EXPECT_GT(Good.LLVM.LoopTransitions, 0U);
    EXPECT_GT(Good.LLVM.RankingChecks, 0U);
    EXPECT_TRUE(Good.Native.Certificate->Relation.NativePreservation);
    EXPECT_EQ(Good.Native.Certificate->Relation.NativeAuditBoundaries.size(),
              Deferred ? 0U : 1U);
    rejected(P.check(R.Residual, IR, {}, Plans, "model", Required),
             Stage::Native);
    for (bool Native : {true, false}) {
      auto Bad = Plans;
      (Native ? Bad.Native : Bad.LLVM)->Cutpoints.front().Rank = {
          NdVar::scalar(0, 8)};
      rejected(P.check(R.Residual, IR, {}, Bad, "model", Required, Policy),
               Native ? Stage::Native : Stage::LLVM);
    }
    auto BadIR = IR;
    BadIR.replace(BadIR.find("ret i64 0"), 9, "ret i64 1");
    const auto BadSource =
        P.check(R.Residual, BadIR, {}, Plans, "model", Required, Policy);
    rejected(BadSource, Stage::LLVM);
    EXPECT_TRUE(BadSource.Native.proved());
    P.Image.Segments[0].Data[4] = 0x75;
    const auto Reachable =
        P.check(R.Residual, IR, {}, Plans, "model", Required, Policy);
    rejected(Reachable, Stage::Native);
    EXPECT_EQ(Reachable.Native.Proof.Status, Status::Unsupported);
  }
}
} // namespace neverd::analysis::llvm_refinement_test

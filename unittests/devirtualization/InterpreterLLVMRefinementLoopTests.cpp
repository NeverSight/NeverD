//===- InterpreterLLVMRefinementLoopTests.cpp - Untrusted loop plans -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
TEST(InterpreterLLVMRefinement, ArbitraryCountRequiresBothInductivePremises) {
  // jrcxz done; lea rcx,[rcx-1]; jmp entry; done: ret.
  Program P({0xe3, 6, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf8, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module(R"(
  entry:
    %rcx = getelementptr i8, ptr %state, i64 8
    %initial = load i64, ptr %rcx, align 8
    br label %loop
  loop:
    %count = phi i64 [%initial, %entry], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub nuw i64 %count, 1
    br label %loop
  exit:
    store i64 0, ptr %rcx, align 8
    ret i64 0
  )");
  InterpreterLLVMRefinementLimits Limits;
  Limits.NativeProof.Execution.MaxBlockVisits = 32;
  const auto Finite = P.check(R.Residual, IR, Limits);
  rejected(Finite, Stage::Native);
  EXPECT_EQ(Finite.Native.Proof.Status, Status::BudgetExceeded);

  InterpreterLLVMRefinementPlans Plans;
  LowIRLoopCutpoint NativeCut;
  NativeCut.OriginalAddress = Entry;
  NativeCut.UseEntryPrefix = true;
  unsigned Matches = 0;
  for (const auto &Origin : R.Origins)
    if (Origin.NativeInstruction.Address == Entry) {
      NativeCut.CandidateAddress = Origin.ResidualAddress;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  const LowIRLoopLocation RCX{LowIRLoopSpace::Register, x86reg::RCX, 8};
  const auto Count = NdVar::tmp(0, 8);
  NativeCut.Inputs = {{LowIRLoopSide::Original, RCX, Count}};
  NativeCut.OriginalState = NativeCut.CandidateState = {{RCX, Count}};
  NativeCut.Rank = {Count};
  Plans.Native = LowIRLoopRefinementPlan{{NativeCut}};

  auto Models =
      prepareInterpreterLLVMRefinement(R.Residual, IR, "model", P.Frame);
  ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
  // Align the guarded headers. A body cut after the nonzero branch would
  // describe a different transition boundary from the LLVM header.
  const auto Left =
      inferLowIRLoopRefinementPlan(Models->Residual.Function, Models->Contract,
                                   {}, {NativeCut.CandidateAddress});
  const auto Right =
      inferLowIRLoopRefinementPlan(Models->LLVM.Function, Models->Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  ASSERT_EQ(Left.Plan->Cutpoints.size(), 1U);
  ASSERT_EQ(Right.Plan->Cutpoints.size(), 1U);
  const auto &A = Left.Plan->Cutpoints.front();
  const auto &B = Right.Plan->Cutpoints.front();
  ASSERT_EQ(A.Rank.size(), 1U);
  ASSERT_EQ(B.Rank.size(), 1U);
  LowIRLoopCutpointPair Pair{A.OriginalAddress, B.OriginalAddress, {}};
  for (const auto &I : A.Inputs)
    for (const auto &J : B.Inputs)
      if (I.Side == LowIRLoopSide::Original && I.Temporary == A.Rank.front() &&
          J.Side == LowIRLoopSide::Original && J.Temporary == B.Rank.front())
        Pair.SharedInputs.push_back({I.Location, J.Location});
  ASSERT_EQ(Pair.SharedInputs.size(), 1U);
  auto Paired = pairLowIRLoopRefinementPlans(*Left.Plan, *Right.Plan, {Pair});
  ASSERT_TRUE(bool(Paired)) << llvm::toString(Paired.takeError());
  Plans.LLVM = std::move(*Paired);
  const auto Good = P.check(R.Residual, IR, {}, Plans);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Native.Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Certificate->LLVM.Scope,
            LowIRRefinementScope::InductiveLowIRLoops);
  EXPECT_GT(Good.Native.Proof.RankingChecks, 0U);
  EXPECT_GT(Good.LLVM.RankingChecks, 0U);

  for (bool Native : {true, false}) {
    auto Bad = Plans;
    (Native ? Bad.Native : Bad.LLVM)->Cutpoints.front().Rank = {
        NdVar::scalar(0, 8)};
    rejected(P.check(R.Residual, IR, {}, Bad),
             Native ? Stage::Native : Stage::LLVM);
  }
  auto Changed = IR;
  Changed.replace(Changed.find("%count, 1"), 9, "%count, 2");
  rejected(P.check(R.Residual, Changed, {}, Plans), Stage::LLVM);
}

TEST(InterpreterLLVMRefinement, PreservationRestoresEntryAcrossLoopCutpoints) {
  // Save entry RBX in RDX, use RBX inside an arbitrary-word countdown, then
  // restore it. The current RBX at a loop cut is not the function-entry RBX.
  // mov rdx,rbx; header: jrcxz done; mov rbx,rcx; lea rcx,[rcx-1];
  // jmp header; done: mov rbx,rdx; ret.
  Program P({0x48, 0x89, 0xda, 0xe3, 9, 0x48, 0x89, 0xcb, 0x48, 0x8d, 0x49,
             0xff, 0xeb, 0xf5, 0x48, 0x89, 0xd3, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module(R"(
  entry:
    %rbx = getelementptr i8, ptr %state, i64 24
    %saved = load i64, ptr %rbx, align 8
    %rdx = getelementptr i8, ptr %state, i64 16
    store i64 %saved, ptr %rdx, align 8
    %rcx = getelementptr i8, ptr %state, i64 8
    %initial = load i64, ptr %rcx, align 8
    %empty = icmp eq i64 %initial, 0
    br i1 %empty, label %exit, label %body
  body:
    %count = phi i64 [%initial, %entry], [%next, %body]
    store i64 %count, ptr %rbx, align 8
    %next = sub i64 %count, 1
    %done = icmp eq i64 %next, 0
    br i1 %done, label %exit, label %body
  exit:
    store i64 0, ptr %rcx, align 8
    store i64 %saved, ptr %rbx, align 8
    ret i64 0
  )");
  InterpreterLLVMRefinementPreservation Required;
  Required.ModeledRegisters = {{x86reg::RBX, 8}};
  Required.NativeState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::SelectedWitness};
  InterpreterLLVMRefinementLimits Limits;
  Limits.NativeProof.Execution.MaxBlockVisits = 32;
  const auto Finite = P.check(R.Residual, IR, Limits, {}, "model", Required);
  rejected(Finite, Stage::Native);
  EXPECT_EQ(Finite.Native.Proof.Status, Status::BudgetExceeded);

  InterpreterLLVMRefinementPlans Plans;
  LowIRLoopCutpoint Cut;
  // Recovery has separate first-entry and backedge copies of the native
  // guard. Its body is unique; the LLVM loop is rotated to the same boundary.
  Cut.OriginalAddress = Entry + 5;
  Cut.UseEntryPrefix = true;
  unsigned Matches = 0;
  for (const auto &Origin : R.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  const LowIRLoopLocation RCX{LowIRLoopSpace::Register, x86reg::RCX, 8};
  const LowIRLoopLocation RBX{LowIRLoopSpace::Register, x86reg::RBX, 8};
  const LowIRLoopLocation RDX{LowIRLoopSpace::Register, x86reg::RDX, 8};
  const auto Count = NdVar::tmp(0, 8);
  const auto CurrentRBX = NdVar::tmp(8, 8);
  const auto SavedRBX = NdVar::tmp(16, 8);
  Cut.Inputs = {{LowIRLoopSide::Original, RCX, Count},
                {LowIRLoopSide::Original, RBX, CurrentRBX},
                {LowIRLoopSide::OriginalPrefix, RDX, SavedRBX}};
  Cut.OriginalState =
      Cut.CandidateState = {{RCX, Count}, {RBX, CurrentRBX}, {RDX, SavedRBX}};
  LowOp Nonzero;
  Nonzero.Opcode = NdOp::INT_NOTEQUAL;
  Nonzero.Output = NdVar::tmp(24, 1);
  Nonzero.addInput(Count);
  Nonzero.addInput(NdVar::scalar(0, 8));
  Cut.Expressions = {Nonzero};
  Cut.Predicate = Nonzero.Output;
  Cut.Rank = {Count};
  Plans.Native = LowIRLoopRefinementPlan{{Cut}};
  auto Models = prepareInterpreterLLVMRefinement(R.Residual, IR, "model",
                                                 P.Frame, {}, Required);
  ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
  const auto Left = inferLowIRLoopRefinementPlan(
      Models->Residual.Function, Models->Contract, {}, {Cut.CandidateAddress});
  // Select the body before its decrement, not the separate PHI-copy edge
  // reached only after an iteration. Both cuts must use the same count.
  va_t LLVMBody = 0;
  Matches = 0;
  for (const auto &Block : Models->LLVM.Function.Blocks)
    if (std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const auto &Op) {
          return Op.Opcode == NdOp::INT_SUB;
        })) {
      LLVMBody = Block.StartAddr;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  const auto Right = inferLowIRLoopRefinementPlan(
      Models->LLVM.Function, Models->Contract, {}, {LLVMBody});
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  ASSERT_EQ(Left.Plan->Cutpoints.size(), 1U);
  ASSERT_EQ(Right.Plan->Cutpoints.size(), 1U);
  const auto &A = Left.Plan->Cutpoints.front();
  const auto &B = Right.Plan->Cutpoints.front();
  ASSERT_EQ(A.Rank.size(), 1U);
  ASSERT_EQ(B.Rank.size(), 1U);
  LowIRLoopCutpointPair Pair{A.OriginalAddress, B.OriginalAddress, {}};
  for (const auto &I : A.Inputs)
    for (const auto &J : B.Inputs)
      if (I.Side == LowIRLoopSide::Original && I.Temporary == A.Rank.front() &&
          J.Side == LowIRLoopSide::Original && J.Temporary == B.Rank.front())
        Pair.SharedInputs.push_back({I.Location, J.Location});
  ASSERT_EQ(Pair.SharedInputs.size(), 1U);
  auto Paired = pairLowIRLoopRefinementPlans(*Left.Plan, *Right.Plan, {Pair});
  ASSERT_TRUE(bool(Paired)) << llvm::toString(Paired.takeError());
  Plans.LLVM = std::move(*Paired);
  const auto Good = P.check(R.Residual, IR, {}, Plans, "model", Required);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Native.Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Certificate->LLVM.Scope,
            LowIRRefinementScope::InductiveLowIRLoops);
  ASSERT_TRUE(Good.Certificate->Native.Relation.NativePreservation);
  EXPECT_EQ(Good.Certificate->Native.Relation.NativePreservation->Quantifier,
            LowIRNativePreservationQuantifier::SelectedWitness);
  EXPECT_GT(Good.Native.Proof.LoopTransitions, 0U);
  EXPECT_GT(Good.LLVM.LoopTransitions, 0U);
  EXPECT_GT(Good.Native.Proof.RankingChecks, 0U);
  EXPECT_GT(Good.LLVM.RankingChecks, 0U);

  // The public search must discover the source relation without the manual
  // cut address or rank binding above. The composite checker then prepares
  // fresh models and rechecks both premises with the same preservation.
  LowIRLoopAlignmentLimits Search;
  Search.MaxRankPairingAttempts = 32;
  const auto Automatic = inferAndCheckLowIRLoopRefinement(
      Models->Residual.Function, Models->Residual.Instructions,
      Models->LLVM.Function, Models->Contract,
      LowIRRefinementWitness::LiftedBits, Search);
  ASSERT_TRUE(Automatic.proved())
      << Automatic.Diagnostic << ": " << Automatic.LastCandidateDiagnostic;
  EXPECT_GT(Automatic.RankPairingAttempts, 0U);
  Plans.LLVM = Automatic.Refinement.Certificate->LoopPlan;
  const auto Composed = P.check(R.Residual, IR, {}, Plans, "model", Required);
  ASSERT_TRUE(Composed.proved()) << Composed.Diagnostic;
  const auto &Observed = Composed.Certificate->LLVM.Contract.ReturnRegisters;
  ASSERT_EQ(Observed.size(), 18U);
  for (unsigned I = 0; I != 17; ++I) {
    EXPECT_EQ(Observed[I].Offset, 8U * I);
    EXPECT_EQ(Observed[I].Bytes, 8U);
  }
  EXPECT_EQ(Observed.back().Offset, LLVMInterpreterDefinednessOffset);
  EXPECT_EQ(Observed.back().Bytes, 1U);
  EXPECT_TRUE(Composed.Certificate->Native.Relation.NativePreservation);

  auto Changed = IR;
  Changed.replace(Changed.find("ret i64 0"), 9,
                  "%ran = icmp ne i64 %initial, 0\n"
                  "    %status = zext i1 %ran to i64\n"
                  "    ret i64 %status");
  const auto BadSource =
      P.check(R.Residual, Changed, {}, Plans, "model", Required);
  rejected(BadSource, Stage::LLVM);
  EXPECT_TRUE(BadSource.Native.proved());
  EXPECT_EQ(BadSource.LLVM.Status, Status::Different) << BadSource.Diagnostic;
  for (bool Native : {true, false}) {
    auto BadPlans = Plans;
    (Native ? BadPlans.Native : BadPlans.LLVM)->Cutpoints.front().Rank = {
        NdVar::scalar(0, 8)};
    rejected(P.check(R.Residual, IR, {}, BadPlans, "model", Required),
             Native ? Stage::Native : Stage::LLVM);
  }
  P.Image.Segments.front().Data[16] = 0xc3; // mov rbx,rax, not saved entry RBX.
  const auto BadNative = P.check(R.Residual, IR, {}, Plans, "model", Required);
  rejected(BadNative, Stage::Native);
  EXPECT_FALSE(BadNative.Native.Certificate);
  EXPECT_EQ(BadNative.LLVM.Operations, 0U);
}
} // namespace neverd::analysis::llvm_refinement_test

//===- LLVMInterpreterModelLoopTests.cpp - PHI and complete loop checks
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, BackedgePhiCopiesAreParallel) {
  parse(R"(
    %second = getelementptr i8, ptr %state, i64 8
    %x = load i64, ptr %state, align 8
    %y = load i64, ptr %second, align 8
    br label %loop
  loop:
    %a = phi i64 [%x, %0], [%b, %body]
    %b = phi i64 [%y, %0], [%a, %body]
    %count = phi i64 [2, %0], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub nuw i64 %count, 1
    br label %loop
  exit:
    store i64 %a, ptr %state, align 8
    store i64 %b, ptr %second, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle()); // Two swaps preserve both arbitrary 64-bit inputs.
  expect(Oracle({op(NdOp::COPY, r(0), {r(8)})}), Status::Different);
}

TEST_F(LLVMModel, ArbitraryWordCountNeedsACompleteInductiveCheck) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    br label %loop
  loop:
    %count = phi i64 [%x, %0], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub i64 %count, 1
    br label %loop
  exit:
    store i64 0, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  auto M = model();
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  const auto C = llvmInterpreterMachineStateContract();
  const auto Proposal = inferLowIRLoopRefinementPlan(M->Function, C);
  ASSERT_TRUE(Proposal.inferred()) << Proposal.Diagnostic;
  const auto Proof = checkLowIRLoopRefinement(
      M->Function, M->Instructions, M->Function, C, *Proposal.Plan,
      LowIRRefinementWitness::LiftedBits);
  EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
  auto Wrong = M->Function;
  for (auto &B : Wrong.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::RETURN)
        O.Inputs[0] = n(1);
  const auto Rejected = checkLowIRLoopRefinement(
      M->Function, M->Instructions, Wrong, C, *Proposal.Plan,
      LowIRRefinementWitness::LiftedBits);
  EXPECT_EQ(Rejected.Status, Status::Different) << Rejected.Diagnostic;
  EXPECT_FALSE(Rejected.Certificate);
}

TEST_F(LLVMModel, RejectedBodyTemplateDoesNotHideAValidGuardedHeader) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    br label %loop
  loop:
    %count = phi i64 [%x, %0], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub nuw i64 %count, 1
    br label %loop
  exit:
    store i64 0, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  auto M = model();
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  const auto C = llvmInterpreterMachineStateContract();
  const auto Inferred = inferLowIRLoopRefinementPlan(M->Function, C);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  EXPECT_GT(Inferred.CutpointAttempts, 1U);
  ASSERT_EQ(Inferred.Plan->Cutpoints.size(), 1U);
  LowIRLoopAlignmentLimits Alignment;
  Alignment.MaxCuts = 1;
  // The SSA counter is a register input. Frame-only pairing cannot relate
  // the two independent counters, even when both models are identical.
  const auto Unpaired = inferAndCheckLowIRLoopRefinement(
      M->Function, M->Instructions, M->Function, C,
      LowIRRefinementWitness::LiftedBits, Alignment);
  EXPECT_FALSE(Unpaired.proved());
  EXPECT_FALSE(Unpaired.Refinement.Certificate);
  EXPECT_EQ(Unpaired.RankPairingAttempts, 0U);
  Alignment.MaxRankPairingAttempts = 1;
  const auto Aligned = inferAndCheckLowIRLoopRefinement(
      M->Function, M->Instructions, M->Function, C,
      LowIRRefinementWitness::LiftedBits, Alignment);
  ASSERT_TRUE(Aligned.proved())
      << Aligned.Diagnostic << ": " << Aligned.LastCandidateDiagnostic;
  ASSERT_TRUE(Aligned.Refinement.Certificate);
  EXPECT_EQ(Aligned.RankPairingAttempts, 1U);
  const auto Proof = checkLowIRLoopRefinement(
      M->Function, M->Instructions, M->Function, C, *Inferred.Plan,
      LowIRRefinementWitness::LiftedBits);
  EXPECT_TRUE(Proof.proved()) << Proof.Diagnostic;
  for (bool QueryBudget : {false, true}) {
    LowIRLoopInferenceLimits Limits;
    if (QueryBudget)
      Limits.Execution.MaxSolverQueries = Inferred.SolverQueries - 1;
    else
      Limits.MaxCutpointAttempts = 1;
    const auto Limited = inferLowIRLoopRefinementPlan(M->Function, C, Limits);
    EXPECT_EQ(Limited.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Limited.Diagnostic;
    EXPECT_FALSE(Limited.Plan);
    if (QueryBudget)
      EXPECT_LE(Limited.SolverQueries, Limits.Execution.MaxSolverQueries);
    else
      EXPECT_EQ(Limited.CutpointAttempts, 1U);
  }
}

TEST_F(LLVMModel, ActualEntryContractFailureDoesNotRetryCuts) {
  parse(R"(
    %bad = add nuw i64 -1, 1
    %x = load i64, ptr %state, align 8
    br label %loop
  loop:
    %count = phi i64 [%x, %0], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub i64 %count, 1
    br label %loop
  exit:
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  auto M = model();
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  const auto Inferred = inferLowIRLoopRefinementPlan(
      M->Function, llvmInterpreterMachineStateContract());
  EXPECT_EQ(Inferred.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Plan);
  EXPECT_EQ(Inferred.CutpointAttempts, 1U);
  EXPECT_NE(Inferred.Diagnostic.find("preserve entry register"),
            std::string::npos);
}
} // namespace neverd::analysis::llvm_model_test

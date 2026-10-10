//===- InterpreterLLVMRefinementPreservationTests.cpp
//----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

#include <algorithm>
#include <array>
#include <limits>

namespace neverd::analysis::llvm_refinement_test {
namespace {
using Preservation = InterpreterLLVMRefinementPreservation;

Preservation preserveRBX(bool Opaque = true) {
  Preservation P;
  P.ModeledRegisters = {{x86reg::RBX, 8}};
  if (Opaque)
    P.NativeState = LowIRNativePreservationRequirement{
        LowPreservedStateSet::LegacyIntegerOpaqueV1,
        LowIRNativePreservationQuantifier::SelectedWitness};
  return P;
}

void ranges(llvm::ArrayRef<symbolic::SymRegisterRange> Actual,
            llvm::ArrayRef<symbolic::SymRegisterRange> Expected) {
  ASSERT_EQ(Actual.size(), Expected.size());
  for (size_t I = 0; I != Actual.size(); ++I) {
    EXPECT_EQ(Actual[I].Offset, Expected[I].Offset);
    EXPECT_EQ(Actual[I].Bytes, Expected[I].Bytes);
  }
}

std::string clobberRBX() {
  return module(R"(
    %rbx = getelementptr i8, ptr %state, i64 24
    store i64 7, ptr %rbx, align 8
    ret i64 0
  )");
}
} // namespace

TEST(InterpreterLLVMRefinement, PreservationNormalizesOnlyRequestedGPRBytes) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  auto Required = preserveRBX();
  Required.ModeledRegisters = {{25, 2}, {26, 3},  {31, 2},
                               {33, 1}, {127, 1}, {25, 2}};
  auto Models = prepareInterpreterLLVMRefinement(
      R.Residual, module("ret i64 0"), "model", P.Frame, {}, Required);
  ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
  const std::vector<symbolic::SymRegisterRange> Expected{
      {x86reg::RSP, 8}, {25, 4}, {31, 1}, {127, 1}};
  ranges(Models->Preservation.ModeledRegisters, Expected);
  ASSERT_TRUE(Models->Preservation.NativeState);
  EXPECT_FALSE(Models->Contract.NativePreservedState);
  EXPECT_EQ(Models->Contract.ReturnRegisters.size(), 18U);
  EXPECT_EQ(Models->Contract.EntryConstants.size(), 1U);
  EXPECT_TRUE(Models->Contract.ObserveWrittenFrameBytes);
  ASSERT_EQ(Models->Contract.PreservedRegisters.size(), Expected.size() + 1);
  EXPECT_EQ(Models->Contract.PreservedRegisters[0].Offset,
            LLVMInterpreterDefinednessOffset);
  ranges(llvm::ArrayRef(Models->Contract.PreservedRegisters).drop_front(),
         Expected);
  ASSERT_EQ(Models->Contract.PreservedFrameRanges.size(), 1U);
  EXPECT_EQ(Models->Contract.PreservedFrameRanges[0].Offset, 0);
  EXPECT_EQ(Models->Contract.PreservedFrameRanges[0].Bytes, 8);
  const auto Good =
      P.check(R.Residual, module("ret i64 0"), {}, {}, "model", Required);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  ranges(Good.Certificate->Native.Relation.Contract.PreservedRegisters,
         Expected);
  ranges(llvm::ArrayRef(Good.Certificate->LLVM.Contract.PreservedRegisters)
             .drop_front(),
         Expected);
  EXPECT_TRUE(Good.Certificate->Native.Relation.NativePreservation);
  EXPECT_FALSE(Good.Certificate->LLVM.NativePreservation);

  Required.ModeledRegisters = {{0, 128}};
  Models = prepareInterpreterLLVMRefinement(R.Residual, module("ret i64 0"),
                                            "model", P.Frame, {}, Required);
  ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
  ASSERT_EQ(Models->Preservation.ModeledRegisters.size(), 16U);
  std::array<bool, 128> Covered{};
  for (auto Range : Models->Preservation.ModeledRegisters) {
    ASSERT_EQ(Range.Bytes, 8);
    ASSERT_LT(Range.Offset, Covered.size());
    EXPECT_EQ(Range.Offset % 8, 0U);
    for (unsigned I = 0; I != Range.Bytes; ++I) {
      EXPECT_FALSE(Covered[Range.Offset + I]);
      Covered[Range.Offset + I] = true;
    }
  }
  EXPECT_TRUE(
      std::all_of(Covered.begin(), Covered.end(), [](bool B) { return B; }));
}

TEST(InterpreterLLVMRefinement, PreservationRejectsMalformedAndUnmodeledBytes) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (auto Range : std::vector<symbolic::SymRegisterRange>{
           {0, 0},
           {std::numeric_limits<uint64_t>::max(), 1},
           {127, 2},
           {128, 1},
           {x86reg::XMM0, 16},
           {x86reg::extendedGeneralReg(16), 8},
           {LLVMInterpreterDefinednessOffset, 1},
           {0, 129}}) {
    SCOPED_TRACE(Range.Offset);
    Preservation Required;
    Required.ModeledRegisters = {Range};
    auto Models = prepareInterpreterLLVMRefinement(
        R.Residual, module("ret i64 0"), "model", P.Frame, {}, Required);
    ASSERT_FALSE(bool(Models));
    EXPECT_NE(llvm::toString(Models.takeError()).find("preservation"),
              std::string::npos);
    const auto Bad =
        P.check(R.Residual, module("ret i64 0"), {}, {}, "model", Required);
    rejected(Bad, Stage::Preparation);
    EXPECT_EQ(Bad.Native.Proof.Operations, 0U);
    EXPECT_EQ(Bad.LLVM.Operations, 0U);
  }
}

TEST(InterpreterLLVMRefinement,
     PreservationConstructionSharesPreparationBudget) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Prepared = [&](const Preservation &Required, uint64_t Budget) {
    InterpreterLLVMRefinementLimits Limits;
    Limits.MaxPreparationItems = Budget;
    auto Models = prepareInterpreterLLVMRefinement(
        R.Residual, module("ret i64 0"), "model", P.Frame, Limits, Required);
    const bool Good = bool(Models);
    if (!Good)
      llvm::consumeError(Models.takeError());
    return Good;
  };
  const auto Minimum = [&](const Preservation &Required) {
    uint64_t Low = 0,
             High = InterpreterLLVMRefinementLimits{}.MaxPreparationItems;
    EXPECT_TRUE(Prepared(Required, High));
    while (Low != High) {
      const auto Mid = Low + (High - Low) / 2;
      if (Prepared(Required, Mid))
        High = Mid;
      else
        Low = Mid + 1;
    }
    return Low;
  };
  const auto Default = Minimum({});
  auto Required = preserveRBX();
  Required.ModeledRegisters = {{0, 128}, {x86reg::RBX, 8}};
  const auto Exact = Minimum(Required);
  // Independent construction cost: two ranges, native metadata, coverage
  // initialization and scan, 136 requested bytes, and 15 non-RSP outputs.
  ASSERT_EQ(Exact, Default + 2 + 1 + 2 * 128 + 136 + 15);
  EXPECT_FALSE(Prepared(Required, Default));
  EXPECT_TRUE(Prepared(Required, Exact));
  EXPECT_FALSE(Prepared(Required, Exact - 1));
  Required.ModeledRegisters.assign(Exact + 1, {x86reg::RBX, 1});
  EXPECT_FALSE(Prepared(Required, Exact));
  Required.ModeledRegisters.clear();
  EXPECT_FALSE(
      Prepared(Required, Default)); // Native request metadata also costs work.
}

TEST(InterpreterLLVMRefinement,
     PreservationLeavesEmptyAndRedundantDefaultsStable) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Default = P.check(R.Residual, module("ret i64 0"));
  ASSERT_TRUE(Default.proved()) << Default.Diagnostic;
  for (const auto &Required :
       {Preservation{}, Preservation{{{x86reg::RSP + 1, 1}}, {}}}) {
    const auto Same =
        P.check(R.Residual, module("ret i64 0"), {}, {}, "model", Required);
    ASSERT_TRUE(Same.proved()) << Same.Diagnostic;
    EXPECT_EQ(Same.Certificate->InputDigest, Default.Certificate->InputDigest);
    EXPECT_EQ(Same.Native.Certificate->InputDigest,
              Default.Native.Certificate->InputDigest);
    EXPECT_EQ(Same.LLVM.Certificate->InputDigest,
              Default.LLVM.Certificate->InputDigest);
    EXPECT_EQ(Same.Native.Proof.Operations, Default.Native.Proof.Operations);
    EXPECT_EQ(Same.Native.Proof.Observations,
              Default.Native.Proof.Observations);
    EXPECT_EQ(Same.Native.Proof.SolverQueries,
              Default.Native.Proof.SolverQueries);
    EXPECT_EQ(Same.LLVM.Operations, Default.LLVM.Operations);
    EXPECT_EQ(Same.LLVM.Observations, Default.LLVM.Observations);
    EXPECT_EQ(Same.LLVM.SolverQueries, Default.LLVM.SolverQueries);
  }
}

TEST(InterpreterLLVMRefinement, PreservationRejectsIdenticalFinalRBXClobbers) {
  Program P({0xbb, 7, 0, 0, 0, 0xc3}); // mov ebx,7; ret.
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(P.check(R.Residual, clobberRBX()).proved());
  auto Required = preserveRBX();
  Required.ModeledRegisters.clear();
  ASSERT_TRUE(
      P.check(R.Residual, clobberRBX(), {}, {}, "model", Required).proved());
  Required.ModeledRegisters = {{x86reg::RBX, 8}};
  const auto Bad = P.check(R.Residual, clobberRBX(), {}, {}, "model", Required);
  rejected(Bad, Stage::Native);
  EXPECT_EQ(Bad.Native.Proof.Status, Status::ContractViolation)
      << Bad.Diagnostic;
  EXPECT_FALSE(Bad.Native.Certificate);
  EXPECT_FALSE(Bad.LLVM.Certificate);
  EXPECT_EQ(Bad.LLVM.Operations, 0U);
}

TEST(InterpreterLLVMRefinement,
     PreservationCannotCopyNativeConstantsIntoSource) {
  Program P({0xbb, 7, 0, 0, 0, 0xc3});
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RBX, 8), 7});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(P.check(R.Residual, clobberRBX()).proved());
  const auto Bad =
      P.check(R.Residual, clobberRBX(), {}, {}, "model", preserveRBX());
  rejected(Bad, Stage::LLVM);
  EXPECT_TRUE(Bad.Native.proved()) << Bad.Diagnostic;
  EXPECT_EQ(Bad.LLVM.Status, Status::ContractViolation) << Bad.Diagnostic;
  EXPECT_FALSE(Bad.LLVM.Certificate);
}

TEST(InterpreterLLVMRefinement, PreservationUsesTheRealEntryValueAfterRestore) {
  // mov rdx,rbx; mov ebx,7; mov rbx,rdx; ret.
  Program P({0x48, 0x89, 0xda, 0xbb, 7, 0, 0, 0, 0x48, 0x89, 0xd3, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module(R"(
    %rbx = getelementptr i8, ptr %state, i64 24
    %saved = load i64, ptr %rbx, align 8
    %rdx = getelementptr i8, ptr %state, i64 16
    store i64 %saved, ptr %rdx, align 8
    ret i64 0
  )");
  const auto Required = preserveRBX();
  const auto Good = P.check(R.Residual, IR, {}, {}, "model", Required);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_TRUE(Good.Certificate->Native.Relation.NativePreservation);
  const auto Default = P.check(R.Residual, IR);
  ASSERT_TRUE(Default.proved()) << Default.Diagnostic;
  EXPECT_NE(Good.Certificate->InputDigest, Default.Certificate->InputDigest);
  auto Changed = IR;
  Changed.replace(Changed.find("ret i64 0"), 9, "ret i64 1");
  const auto BadSource =
      P.check(R.Residual, Changed, {}, {}, "model", Required);
  rejected(BadSource, Stage::LLVM);
  EXPECT_TRUE(BadSource.Native.proved());
  P.Image.Segments.front().Data[10] = 0xc3; // Restore RBX from RAX instead.
  rejected(P.check(R.Residual, IR, {}, {}, "model", Required), Stage::Native);
}

TEST(InterpreterLLVMRefinement,
     PreservationKeepsFreshNativeEvidenceAndWitness) {
  Program P({0x90, 0x90, 0x90, 0x90, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module("ret i64 0");
  auto Required = preserveRBX();
  for (auto Witness :
       {LowIRRefinementWitness::LiftedBits, LowIRRefinementWitness::ZeroBits}) {
    const auto Good =
        checkBinaryLLVMRefinement(P.Image, Entry, P.Options, R.Residual, IR,
                                  "model", P.Frame, {}, Witness, {}, Required);
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    const auto &Evidence = Good.Certificate->Native.Relation.NativePreservation;
    ASSERT_TRUE(Evidence);
    EXPECT_EQ(Evidence->Quantifier,
              LowIRNativePreservationQuantifier::SelectedWitness);
    EXPECT_EQ(Good.Certificate->Native.Relation.Witness, Witness);
    EXPECT_EQ(Evidence->Instructions,
              Good.Certificate->Native.Relation.OriginalInstructions.size());
  }
  Required.NativeState->Quantifier =
      LowIRNativePreservationQuantifier::AllUndefinedChoices;
  const auto All = P.check(R.Residual, IR, {}, {}, "model", Required);
  rejected(All, Stage::Native);
  EXPECT_EQ(All.Native.Proof.Status, Status::Unsupported);
  Required.NativeState->Quantifier =
      LowIRNativePreservationQuantifier::SelectedWitness;
  // Reuse only an arbitrary residual proposal, never old byte authority.
  // PXOR XMM6,XMM6 writes state outside this scalar wrapper's model.
  P.Image.Segments.front().Data = {0x66, 0x0f, 0xef, 0xf6, 0xc3};
  const auto Unsupported = P.check(R.Residual, IR, {}, {}, "model", Required);
  rejected(Unsupported, Stage::Native);
  EXPECT_EQ(Unsupported.Native.Proof.Status, Status::Unsupported);
  EXPECT_EQ(Unsupported.LLVM.Operations, 0U);
}

TEST(InterpreterLLVMRefinement, PreservationRetainsIndependentProofBudgets) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Required = preserveRBX();
  const auto IR = module("ret i64 0");
  const auto Good = P.check(R.Residual, IR, {}, {}, "model", Required);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (bool Native : {true, false}) {
    InterpreterLLVMRefinementLimits Limits;
    auto &Queries = (Native ? Limits.NativeProof : Limits.LLVMProof)
                        .Execution.MaxSolverQueries;
    Queries =
        Native ? Good.Native.Proof.SolverQueries : Good.LLVM.SolverQueries;
    ASSERT_GT(Queries, 0U);
    EXPECT_TRUE(
        P.check(R.Residual, IR, Limits, {}, "model", Required).proved());
    --Queries;
    const auto Bad = P.check(R.Residual, IR, Limits, {}, "model", Required);
    rejected(Bad, Native ? Stage::Native : Stage::LLVM);
    EXPECT_EQ(Native ? Bad.Native.Proof.Status : Bad.LLVM.Status,
              Status::BudgetExceeded);
    if (!Native)
      EXPECT_TRUE(Bad.Native.proved());
  }
}
} // namespace neverd::analysis::llvm_refinement_test

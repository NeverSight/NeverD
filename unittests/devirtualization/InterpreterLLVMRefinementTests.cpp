//===- InterpreterLLVMRefinementTests.cpp - Fresh composed proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
static std::string constantResult() {
  return module("store i64 7, ptr %state, align 8\nret i64 0");
}

TEST(InterpreterLLVMRefinement,
     ExplicitFrameBoundsMatchAndBindTheNativeContract) {
  Program P({0xb8, 7, 0, 0, 0, 0xeb, 0, 0xc3});
  P.Options.MaxChainedTransfers = 3;
  const auto Original = P.recover();
  ASSERT_TRUE(Original.complete()) << Original.Diagnostic;
  const auto Legacy = P.check(Original.Residual, constantResult());
  ASSERT_TRUE(Legacy.proved()) << Legacy.Diagnostic;
  P.Options.EntryFrameBounds =
      SpecializationEntryFrameBounds{P.Frame.Begin, P.Frame.End};
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto A = P.check(R.Residual, constantResult());
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  EXPECT_NE(A.Native.Certificate->InputDigest,
            Legacy.Native.Certificate->InputDigest);
  const auto Wrong = P.check(
      R.Residual, module("store i64 9, ptr %state, align 8\nret i64 0"));
  rejected(Wrong, Stage::LLVM);
  EXPECT_TRUE(Wrong.Native.proved());
  EXPECT_EQ(Wrong.LLVM.Status, Status::Different);
  P.Options.EntryFrameBounds->Begin = -80;
  const auto Mismatch = P.check(R.Residual, constantResult());
  rejected(Mismatch, Stage::Native);
  EXPECT_EQ(Mismatch.Native.Proof.Status, Status::Invalid);
  P.Frame.Begin = -80;
  const auto B = P.check(R.Residual, constantResult());
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_NE(A.Native.Certificate->InputDigest,
            B.Native.Certificate->InputDigest);
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
}

TEST(InterpreterLLVMRefinement, TransferChainsProveRepeatedNativeCalls) {
  Program P({0xb8, 1, 0, 0, 0, 0xe8, 6,    0,    0, 0,
             0xe8, 1, 0, 0, 0, 0xc3, 0x8d, 0x40, 3, 0xc3});
  P.Options.EntryFrameBounds =
      SpecializationEntryFrameBounds{P.Frame.Begin, P.Frame.End};
  const auto IR = module(R"(
    store i64 7, ptr %state, align 8
    %spword = getelementptr i8, ptr %state, i64 32
    %sp = load i64, ptr %spword, align 8
    %slot = add i64 %sp, -8
    %address = inttoptr i64 %slot to ptr
    store i64 4111, ptr %address, align 1
    ret i64 0
  )");
  for (uint32_t Bound : {0, 1, 16}) {
    P.Options.MaxChainedTransfers = Bound;
    const auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    const auto Proof = P.check(R.Residual, IR);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    const auto MissingStackWrite = P.check(R.Residual, constantResult());
    rejected(MissingStackWrite, Stage::LLVM);
    EXPECT_TRUE(MissingStackWrite.Native.proved());
    EXPECT_EQ(MissingStackWrite.LLVM.Status, Status::Different);
  }
}

TEST(InterpreterLLVMRefinement, EntryAlignmentCannotCertifyAnUnboundDomain) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  P.Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 3};
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Proof = P.check(R.Residual, constantResult());
  rejected(Proof, Stage::Native);
  EXPECT_EQ(Proof.Native.Proof.Status, Status::Unsupported);
  EXPECT_NE(Proof.Diagnostic.find("entry alignment"), std::string::npos);
}

TEST(InterpreterLLVMRefinement, CompleteProofBindsMandatoryObservations) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret.
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Proof = P.check(R.Residual, constantResult());
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  const auto &C = *Proof.Certificate;
  EXPECT_EQ(C.FlagsSemanticsVersion, 1U);
  EXPECT_EQ(C.FunctionName, "model");
  EXPECT_EQ(C.Native.InputDigest, Proof.Native.Certificate->InputDigest);
  EXPECT_EQ(C.LLVM.InputDigest, Proof.LLVM.Certificate->InputDigest);
  EXPECT_EQ(C.Native.Relation.Contract.ReturnRegisters.size(), 23U);
  EXPECT_EQ(C.LLVM.Contract.ReturnRegisters.size(), 18U);
  EXPECT_TRUE(C.LLVM.Contract.ObserveWrittenFrameBytes);
  EXPECT_EQ(C.LLVM.Contract.EntryConstants.size(), 1U);
  EXPECT_EQ(C.LLVM.Contract.PreservedRegisters.size(), 2U);
  EXPECT_EQ(C.LLVM.Contract.PreservedFrameRanges.size(), 1U);
  ASSERT_TRUE(C.Native.Relation.Contract.Frame);
  ASSERT_TRUE(C.LLVM.Contract.Frame);
  EXPECT_EQ(C.Native.Relation.Contract.Frame->ExcludedAddressRanges.size(), 1U);
  EXPECT_TRUE(C.LLVM.Contract.Frame->ExcludedAddressRanges.empty());
}

TEST(InterpreterLLVMRefinement, ExactTextAndSelectedFunctionAreBound) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR =
      module("ret i64 0") + "define i64 @other(ptr %state) { ret i64 0 }\n";
  const auto A = P.check(R.Residual, IR);
  const auto B = P.check(R.Residual, IR + "; distinct serialized artifact\n");
  const auto C = P.check(R.Residual, IR, {}, {}, "other");
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(A.Certificate->LLVMIRDigest, B.Certificate->LLVMIRDigest);
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  EXPECT_EQ(A.Certificate->LLVMIRDigest, C.Certificate->LLVMIRDigest);
  EXPECT_NE(A.Certificate->InputDigest, C.Certificate->InputDigest);
  auto Limits = InterpreterLLVMRefinementLimits{};
  ++Limits.MaxPreparationItems;
  const auto D = P.check(R.Residual, IR, Limits);
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, D.Certificate->InputDigest);
}

TEST(InterpreterLLVMRefinement, NativeBytesAndIdenticalResidualAreRechecked) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  P.Image.Segments[0].Data[1] = 9;
  rejected(P.check(R.Residual, constantResult()), Stage::Native);
  const auto Changed = P.recover();
  ASSERT_TRUE(Changed.complete()) << Changed.Diagnostic;
  P.Image.Segments[0].Data[1] = 7;
  rejected(P.check(Changed.Residual, constantResult()), Stage::Native);
}

TEST(InterpreterLLVMRefinement, ResultFlagsStatusAndFrameCannotBeDropped) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (const std::string &IR :
       {module("store i64 9, ptr %state, align 8\nret i64 0"),
        module("store i64 7, ptr %state, align 8\nret i64 1"), module(R"(
             store i64 7, ptr %state, align 8
             %flags = getelementptr i8, ptr %state, i64 128
             %old = load i64, ptr %flags, align 8
             %changed = xor i64 %old, 1
             store i64 %changed, ptr %flags, align 8
             ret i64 0
           )"),
        module(R"(
             store i64 7, ptr %state, align 8
             %stack = getelementptr i8, ptr %state, i64 32
             %sp = load i64, ptr %stack, align 8
             %address = add i64 %sp, -8
             %p = inttoptr i64 %address to ptr
             store i8 1, ptr %p, align 1
             ret i64 0
           )")}) {
    const auto Proof = P.check(R.Residual, IR);
    rejected(Proof, Stage::LLVM);
    EXPECT_TRUE(Proof.Native.proved()) << Proof.Diagnostic;
    EXPECT_EQ(Proof.LLVM.Status, Status::Different) << Proof.Diagnostic;
  }
}

TEST(InterpreterLLVMRefinement, PoisonAndReturnSlotWritesRefuseCertificates) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (const std::string &IR :
       {module("%poison = add nuw i64 -1, 1\nret i64 0"), module(R"(
             %stack = getelementptr i8, ptr %state, i64 32
             %sp = load i64, ptr %stack, align 8
             %p = inttoptr i64 %sp to ptr
             store i8 1, ptr %p, align 1
             ret i64 0
           )"),
        module(R"(
             %stack = getelementptr i8, ptr %state, i64 32
             store i64 0, ptr %stack, align 8
             ret i64 0
           )")}) {
    const auto Proof = P.check(R.Residual, IR);
    rejected(Proof, Stage::LLVM);
    EXPECT_EQ(Proof.LLVM.Status, Status::ContractViolation) << Proof.Diagnostic;
  }
}

TEST(InterpreterLLVMRefinement, NativeScalarFlagConstantIsNotPackedFlags) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  // Physical CF aliases raw packed RFLAGS numerically, but the namespaces
  // have different meanings. Source entry constants must not be copied.
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::CF, 1), 1});
  const auto Good = P.check(R.Residual, module("ret i64 0"));
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  const auto Bad = P.check(R.Residual, module(R"(
    %flags = getelementptr i8, ptr %state, i64 128
    store i64 3, ptr %flags, align 8
    ret i64 0
  )"));
  rejected(Bad, Stage::LLVM);
  EXPECT_EQ(Bad.LLVM.Status, Status::Different) << Bad.Diagnostic;
}

TEST(InterpreterLLVMRefinement, SourceDomainRemainsBroaderThanNativeConstants) {
  Program P({0x48, 0x89, 0xc8, 0xc3}); // mov rax,rcx; ret.
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RCX, 8), 7});
  const auto Bad = P.check(R.Residual, constantResult());
  rejected(Bad, Stage::LLVM);
  EXPECT_TRUE(Bad.Native.proved()) << Bad.Diagnostic;
  EXPECT_EQ(Bad.LLVM.Status, Status::Different) << Bad.Diagnostic;
  const auto Good = P.check(R.Residual, module(R"(
    %rcx = getelementptr i8, ptr %state, i64 8
    %value = load i64, ptr %rcx, align 8
    store i64 %value, ptr %state, align 8
    ret i64 0
  )"));
  EXPECT_TRUE(Good.proved()) << Good.Diagnostic;
}

TEST(InterpreterLLVMRefinement, NativeAndLLVMProofBudgetsRemainIndependent) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (bool Native : {true, false}) {
    InterpreterLLVMRefinementLimits Limits;
    (Native ? Limits.NativeProof : Limits.LLVMProof)
        .Execution.MaxSolverQueries = 0;
    const auto Proof = P.check(R.Residual, module("ret i64 0"), Limits);
    rejected(Proof, Native ? Stage::Native : Stage::LLVM);
    EXPECT_EQ(Native ? Proof.Native.Proof.Status : Proof.LLVM.Status,
              Status::BudgetExceeded)
        << Proof.Diagnostic;
  }
}

TEST(InterpreterLLVMRefinement, PreparationRejectsMalformedAndOversizedInputs) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (const auto &IR :
       {std::string("not LLVM"), module("ret i32 0"),
        module("ret i64 0", "missing"), std::string("declare i64 @model(ptr)"),
        module("call void asm sideeffect \"\", \"\"()\n"
               "ret i64 0")})
    rejected(P.check(R.Residual, IR), Stage::Preparation);
  for (unsigned Which = 0; Which != 5; ++Which) {
    InterpreterLLVMRefinementLimits L;
    switch (Which) {
    case 0:
      L.MaxIRBytes = 1;
      break;
    case 1:
      L.MaxMachineStateOperations = 1;
      break;
    case 2:
      L.MaxPreparationItems = 1;
      break;
    case 3:
      L.LLVMModel.MaxBlocks = 1;
      break;
    case 4:
      L.LLVMModel.MaxOperations = 1;
      break;
    }
    rejected(P.check(R.Residual, module("ret i64 0"), L), Stage::Preparation);
  }
  InterpreterLLVMRefinementLimits L;
  L.MaxIRBytes = 256;
  rejected(
      P.check(R.Residual, module("ret i64 0"), L, {}, std::string(257, 'a')),
      Stage::Preparation);
}

TEST(InterpreterLLVMRefinement, ExplicitProfileAndValidSharedFrameAreRequired) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (unsigned Which = 0; Which != 8; ++Which) {
    auto Bad = P;
    switch (Which) {
    case 0:
      Bad.Options.ExplicitMachineState = false;
      break;
    case 1:
      Bad.Options.NormalNonfaultingExecution = false;
      break;
    case 2:
      Bad.Options.X64CetDisabled = false;
      break;
    case 3:
      Bad.Options.X64FlagsProfile.reset();
      break;
    case 4:
      Bad.Frame.RootRegister.Offset = x86reg::RAX;
      break;
    case 5:
      Bad.Frame.End = 7;
      break;
    case 6:
      Bad.Frame.ExcludedAddressRanges.push_back({9, 9});
      break;
    case 7:
      Bad.Frame.ExcludedAddressRanges.push_back({9, 1});
      break;
    }
    rejected(Bad.check(R.Residual, module("ret i64 0")), Stage::Preparation);
  }
  P.Frame.ExcludedAddressRanges = {{0x2000, 0x2100}, {0x2080, 0x2200}};
  const auto Good = P.check(R.Residual, module("ret i64 0"));
  EXPECT_TRUE(Good.proved()) << Good.Diagnostic;
}

TEST(InterpreterLLVMRefinement, PreparationCannotRepairDanglingEntryEdges) {
  Program P({0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (bool Successor : {true, false}) {
    auto F = R.Residual;
    int MaxId = -1;
    for (const auto &B : F.Blocks)
      MaxId = std::max(MaxId, B.Id);
    auto &Last = F.Blocks.back();
    (Successor ? Last.Succs : Last.Preds).push_back(MaxId + 1);
    auto Prepared = prepareInterpreterLLVMRefinement(F, module("ret i64 0"),
                                                     "model", P.Frame);
    EXPECT_FALSE(bool(Prepared));
    llvm::consumeError(Prepared.takeError());
  }
  auto F = R.Residual;
  F.ModuleAnalysisRoots.insert(0x9000);
  auto Prepared = prepareInterpreterLLVMRefinement(F, module("ret i64 0"),
                                                   "model", P.Frame);
  EXPECT_FALSE(bool(Prepared));
  llvm::consumeError(Prepared.takeError());
}
} // namespace neverd::analysis::llvm_refinement_test

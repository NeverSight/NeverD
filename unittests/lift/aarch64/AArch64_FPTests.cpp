#include "AArch64HighCBehavior.h"

class AArch64_FP : public AArch64HighCBehaviorTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.6-a+bf16", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_fp_a64.o"; }

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string functionSource(const std::string &Source,
                                  const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_FP, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_fp_a64.o not built";
  verifyAllStages(testObj());
}

TEST_F(AArch64_FP, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(AArch64_FP, FaddLifts) {
  verifyLowIRContains(testObj(), "test_fadd_a64", "FLOAT_ADD");
}

TEST_F(AArch64_FP, FsubLifts) {
  verifyLowIRContains(testObj(), "test_fsub_a64", "FLOAT_SUB");
}

TEST_F(AArch64_FP, FmulLifts) {
  verifyLowIRContains(testObj(), "test_fmul_a64", "FLOAT_MULT");
}

TEST_F(AArch64_FP, FdivLifts) {
  verifyLowIRContains(testObj(), "test_fdiv_a64", "FLOAT_DIV");
}

TEST_F(AArch64_FP, FsqrtLifts) {
  verifyLowIRContains(testObj(), "test_fsqrt_a64", "FLOAT_SQRT");
}

TEST_F(AArch64_FP, FnegLifts) {
  verifyLowIRContains(testObj(), "test_fneg_a64", "FLOAT_NEG");
}

TEST_F(AArch64_FP, FabsLifts) {
  verifyLowIRContains(testObj(), "test_fabs_a64", "FLOAT_ABS");
}

TEST_F(AArch64_FP, ScvtfLifts) {
  verifyLowIRContains(testObj(), "test_scvtf_a64", "FLOAT_INT2FLOAT");
}

TEST_F(AArch64_FP, FcvtzsLifts) {
  verifyLowIRContains(testObj(), "test_fcvtzs_a64", "FLOAT_FLOAT2INT");
}

TEST_F(AArch64_FP, FjcvtzsUpdatesExactnessFlag) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.fjcvtzs"), std::string::npos) << F;
  EXPECT_NE(F.find("sitofp i32"), std::string::npos) << F;
  EXPECT_NE(F.find("fcmp oeq double"), std::string::npos) << F;
  EXPECT_EQ(F.find("ret i64 1"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FjcvtzsHighCUsesBuiltinAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__jcvt(__builtin_bit_cast(double"), std::string::npos)
      << source;
  EXPECT_NE(source.find("0x8000000000000000"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
  auto F = functionSource(source, "test_fjcvtzs_z_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("int32_t test_fjcvtzs_z_a64"), std::string::npos) << F;
  expectPairedClangSyntax(cFile, source);

  llvm::LLVMContext Context;
  auto Module = compileHighCFlow(cFile, "armv8.6-a+bf16", Context);
  ASSERT_TRUE(Module);
  const auto *Function = Module->getFunction("test_fjcvtzs_z_a64");
  ASSERT_TRUE(Function);
  ASSERT_EQ(Function->arg_size(), 1u);
  a64_highc_test::ValueFlow Flow(*Function);
  ASSERT_TRUE(Flow.valid());
  auto Converted = a64_highc_test::calls(*Function, "llvm.aarch64.fjcvtzs");
  ASSERT_EQ(Converted.size(), 1u);
  EXPECT_EQ(Flow.origin(Converted[0]->getArgOperand(0)), Function->getArg(0));
  const llvm::Value *SignedConversion = nullptr;
  const llvm::Value *Comparison = nullptr;
  for (const auto &Instruction : Function->front()) {
    if (const auto *Convert = llvm::dyn_cast<llvm::SIToFPInst>(&Instruction)) {
      ASSERT_FALSE(SignedConversion);
      SignedConversion = Convert;
      EXPECT_EQ(Flow.origin(Convert->getOperand(0)), Converted[0]);
    } else if (const auto *Call =
                   llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
      if (Call->getCalledFunction()->getName().starts_with(
              "llvm.experimental.constrained.sitofp")) {
        ASSERT_FALSE(SignedConversion);
        SignedConversion = Call;
        EXPECT_EQ(Flow.origin(Call->getArgOperand(0)), Converted[0]);
      }
    }
  }
  ASSERT_TRUE(SignedConversion);
  for (const auto &Instruction : Function->front()) {
    const llvm::Value *Left = nullptr, *Right = nullptr;
    if (const auto *Compare = llvm::dyn_cast<llvm::FCmpInst>(&Instruction)) {
      EXPECT_EQ(Compare->getPredicate(), llvm::CmpInst::FCMP_OEQ);
      Left = Compare->getOperand(0);
      Right = Compare->getOperand(1);
    } else if (const auto *Call =
                   llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
      if (Call->getCalledFunction()->getName().starts_with(
              "llvm.experimental.constrained.fcmp")) {
        EXPECT_TRUE(
            a64_highc_test::metadataText(Call->getArgOperand(2), "oeq"));
        Left = Call->getArgOperand(0);
        Right = Call->getArgOperand(1);
      }
    }
    if (Left && Right) {
      ASSERT_FALSE(Comparison);
      Comparison = &Instruction;
      EXPECT_TRUE((Flow.origin(Left) == Function->getArg(0) &&
                   Flow.origin(Right) == SignedConversion) ||
                  (Flow.origin(Right) == Function->getArg(0) &&
                   Flow.origin(Left) == SignedConversion));
    }
  }
  ASSERT_TRUE(Comparison);
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Function->front().getTerminator());
  ASSERT_TRUE(Return);
  // Ordered equality must control the result, except that negative zero is
  // never exact. This exercises both truth values through the actual return
  // expression and catches a constant return or a disconnected flag value.
  for (uint64_t Bits :
       {UINT64_C(0), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
        UINT64_C(0x8000000000000000)}) {
    for (bool Equal : {false, true}) {
      auto Result = Flow.integer(Return->getReturnValue(),
                                 {{Function->getArg(0), llvm::APInt(64, Bits)},
                                  {Comparison, llvm::APInt(1, Equal)}});
      ASSERT_TRUE(Result);
      EXPECT_EQ(*Result,
                llvm::APInt(32, Equal && Bits != UINT64_C(0x8000000000000000)));
    }
  }
}

TEST_F(AArch64_FP, FrecpxUsesDedicatedLLVMIntrinsic) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_frecpx_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.neon.frecpx.f32"), std::string::npos) << F;
  EXPECT_EQ(F.find("@llvm.aarch64.neon.frecpe"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FrecpxHighCUsesACLEAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  auto F = functionSource(source, "test_frecpx_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("float test_frecpx_a64"), std::string::npos) << F;
  EXPECT_NE(F.find("vrecpxs_f32"), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_bit_cast(float"), std::string::npos) << F;
  EXPECT_EQ(F.find("vrecpe"), std::string::npos) << F;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_FP, BfmmlaUsesDedicatedLLVMIntrinsic) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_bfmmla_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.neon.bfmmla"), std::string::npos) << F;
  EXPECT_EQ(F.find("fmul double"), std::string::npos) << F;
}

TEST_F(AArch64_FP, BfmmlaHighCUsesACLEAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  auto F = functionSource(source, "test_bfmmla_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("vbfmmlaq_f32"), std::string::npos) << F;
  EXPECT_NE(F.find("bfloat16x8_t"), std::string::npos) << F;
  EXPECT_EQ(F.find("__neverd"), std::string::npos) << F;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_FP, FrintiPreservesDynamicFPCRInLLVMIR) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_frinti_fpcr_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.aarch64.get.fpcr"), std::string::npos) << F;
  EXPECT_NE(F.find("@llvm.aarch64.set.fpcr"), std::string::npos) << F;
  EXPECT_NE(F.find("@llvm.experimental.constrained.nearbyint.f64"),
            std::string::npos)
      << F;
  EXPECT_NE(F.find("metadata !\"round.dynamic\""), std::string::npos) << F;
  EXPECT_EQ(F.find("@llvm.roundeven.f64"), std::string::npos) << F;
}

TEST_F(AArch64_FP, FrintiHighCUsesClangBuiltinsAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("#pragma STDC FENV_ACCESS ON"), std::string::npos)
      << source;
  auto F = functionSource(source, "test_frinti_fpcr_a64");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("__builtin_arm_rsr64(\"FPCR\")"), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_arm_wsr64(\"FPCR\""), std::string::npos) << F;
  EXPECT_NE(F.find("__builtin_elementwise_nearbyint"), std::string::npos) << F;
  EXPECT_EQ(F.find("__neverd"), std::string::npos) << F;
  expectPairedClangSyntax(cFile, source);

  llvm::LLVMContext Context;
  auto Module = compileHighCFlow(cFile, "armv8.6-a+bf16", Context);
  ASSERT_TRUE(Module);
  const auto *Function = Module->getFunction("test_frinti_fpcr_a64");
  ASSERT_TRUE(Function);
  ASSERT_EQ(Function->arg_size(), 1u);
  a64_highc_test::ValueFlow Flow(*Function);
  ASSERT_TRUE(Flow.valid());
  auto Reads = a64_highc_test::calls(*Function, "llvm.read_volatile_register");
  if (Reads.empty())
    Reads = a64_highc_test::calls(*Function, "llvm.read_register");
  auto Writes = a64_highc_test::calls(*Function, "llvm.write_register");
  auto Rounds = a64_highc_test::calls(*Function, "llvm.nearbyint");
  if (Rounds.empty())
    Rounds = a64_highc_test::calls(*Function,
                                   "llvm.experimental.constrained.nearbyint");
  ASSERT_EQ(Reads.size(), 1u);
  ASSERT_EQ(Writes.size(), 2u);
  ASSERT_EQ(Rounds.size(), 1u);
  EXPECT_TRUE(a64_highc_test::metadataText(Reads[0]->getArgOperand(0), "fpcr"));
  for (const auto *Write : Writes)
    EXPECT_TRUE(a64_highc_test::metadataText(Write->getArgOperand(0), "fpcr"));
  EXPECT_TRUE(Reads[0]->comesBefore(Writes[0]));
  EXPECT_TRUE(Writes[0]->comesBefore(Rounds[0]));
  EXPECT_TRUE(Rounds[0]->comesBefore(Writes[1]));
  EXPECT_EQ(Flow.origin(Rounds[0]->getArgOperand(0)), Function->getArg(0));
  EXPECT_EQ(Flow.origin(Writes[1]->getArgOperand(1)), Reads[0]);
  if (Rounds[0]->getCalledFunction()->getName().starts_with(
          "llvm.experimental.constrained."))
    EXPECT_TRUE(a64_highc_test::metadataText(Rounds[0]->getArgOperand(1),
                                             "round.dynamic"));
  // Setting toward-zero rounding changes only FPCR[23:22]; every other bit
  // must survive, and the subsequent write must restore the original value.
  for (uint64_t Saved : {UINT64_C(0), UINT64_MAX, UINT64_C(0x123456789abcdef0),
                         UINT64_C(1) << 22, UINT64_C(2) << 22}) {
    auto Mode = Flow.integer(Writes[0]->getArgOperand(1),
                             {{Reads[0], llvm::APInt(64, Saved)}});
    ASSERT_TRUE(Mode);
    EXPECT_EQ(*Mode, llvm::APInt(64, Saved | (UINT64_C(3) << 22)));
  }
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Function->front().getTerminator());
  ASSERT_TRUE(Return);
  EXPECT_TRUE(Writes[1]->comesBefore(Return));
  EXPECT_EQ(Flow.origin(Return->getReturnValue()), Rounds[0])
      << "return must use the rounding snapshot taken before FPCR restoration";
}

TEST_F(AArch64_FP, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

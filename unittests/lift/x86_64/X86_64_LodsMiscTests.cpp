#include "NeverDLiftFixture.h"

class X86_64_LodsMisc : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_lods_misc.o";
}

TEST_F(X86_64_LodsMisc, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_lods_misc.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_LodsMisc, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_LodsMisc, NoUnreachable) { verifyLLVMIRNoUnreachable(testObj()); }

TEST_F(X86_64_LodsMisc, LodsbPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INTRINSIC") || r.contains("LOAD"))
      << "LODSB should produce load or intrinsic";
}

TEST_F(X86_64_LodsMisc, PushfPopfPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("STORE") || r.contains("LOAD"))
      << "PUSHF/POPF should produce stack ops";
}

TEST_F(X86_64_LodsMisc, CbwCwdePreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INT_SEXT") || r.contains("COPY"))
      << "CBW/CWDE should produce sign extension ops";
}

TEST_F(X86_64_LodsMisc, LoopPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("INT_SUB") || r.contains("COND_BR"))
      << "LOOP should decrement ECX and branch";
}

TEST_F(X86_64_LodsMisc, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_64_LodsMisc, NoConstantTrueBranch) {
  verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_64_LodsMisc, LLVMIRHasConditionalLogic) {
  // Deep optimization proves test_loop_insn(count) == count and removes the
  // two fixed, side-effect-free LOOP variants.  Inspect lowering before that
  // legal CFG elimination; the optimized path remains covered by
  // AllStagesPass, LLVMIRNoVerifierErrors, and NoConstantTrueBranch.
  auto R = liftToLLVMIRUnopt(testObj());
  ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
  bool HasCond = R.out.find("br i1 %") != std::string::npos ||
                 R.out.find("select i1") != std::string::npos ||
                 R.out.find("icmp") != std::string::npos;
  EXPECT_TRUE(HasCond)
      << "Expected conditional logic (br i1/select/icmp) in LLVM IR";
}

TEST_F(X86_64_LodsMisc, DecompileSucceeds) {
  verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_64_LodsMisc, AllModesSucceed) { verifyAllModesSucceed(testObj()); }

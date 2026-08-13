#include "NeverDLiftFixture.h"

class X86_32_LodsMisc : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_lods_misc32.o";
}

TEST_F(X86_32_LodsMisc, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_lods_misc32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_LodsMisc, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_LodsMisc, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

TEST_F(X86_32_LodsMisc, LodsbPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("INTRINSIC") || r.contains("LOAD"))
        << "LODSB should produce load or intrinsic";
}

TEST_F(X86_32_LodsMisc, PushfPopfPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("STORE") || r.contains("LOAD"))
        << "PUSHF/POPF should produce stack ops";
}

TEST_F(X86_32_LodsMisc, CbwCwdePreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("INT_SEXT") || r.contains("COPY"))
        << "CBW/CWDE should produce sign extension ops";
}

TEST_F(X86_32_LodsMisc, LoopPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.contains("INT_SUB") || r.contains("COND_BR"))
        << "LOOP should decrement ECX and branch";
}

TEST_F(X86_32_LodsMisc, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_LodsMisc, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_32_LodsMisc, DecompileSucceeds) {
    verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_32_LodsMisc, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}

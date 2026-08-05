#include "NeverDLiftFixture.h"

class X86_32_String : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_string32.o";
}

TEST_F(X86_32_String, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_string32.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_32_String, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_String, MovsbLifted) {
    verifyLowIRContains(testObj(), "test_movsb32", "INTRINSIC");
}

TEST_F(X86_32_String, StosbLifted) {
    verifyLowIRContains(testObj(), "test_stosb32", "INTRINSIC");
}

TEST_F(X86_32_String, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_String, DecompileProducesOutput) {
    verifyDecompileProducesOutput(testObj());
}

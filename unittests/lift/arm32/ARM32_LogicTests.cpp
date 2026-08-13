#include "NeverDLiftFixture.h"

class ARM32_Logic : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(ARM32_Logic, AllStages) { verifyAllStages(obj("test_logic_arm.o")); }

TEST_F(ARM32_Logic, NoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(obj("test_logic_arm.o"));
}

TEST_F(ARM32_Logic, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_logic_arm.o"));
}

TEST_F(ARM32_Logic, PreservesAndSemantic) {
    verifyLLVMIRContains(obj("test_logic_arm.o"), "test_and_arm", "and");
}

TEST_F(ARM32_Logic, PreservesOrSemantic) {
    verifyLLVMIRContains(obj("test_logic_arm.o"), "test_orr_arm", "or");
}

TEST_F(ARM32_Logic, DecompileC) {
    verifyDecompileProducesOutput(obj("test_logic_arm.o"));
}

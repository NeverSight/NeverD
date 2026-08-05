#include "NeverDLiftFixture.h"

class ARM32_MulDiv : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(ARM32_MulDiv, AllStages) { verifyAllStages(obj("test_muldiv_arm.o")); }

TEST_F(ARM32_MulDiv, NoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(obj("test_muldiv_arm.o"));
}

TEST_F(ARM32_MulDiv, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_muldiv_arm.o"));
}

TEST_F(ARM32_MulDiv, PreservesMulSemantic) {
    verifyLLVMIRContains(obj("test_muldiv_arm.o"), "test_mul_arm", "mul");
}

TEST_F(ARM32_MulDiv, DecompileC) {
    verifyDecompileProducesOutput(obj("test_muldiv_arm.o"));
}

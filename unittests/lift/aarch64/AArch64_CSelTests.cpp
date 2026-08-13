#include "NeverDLiftFixture.h"

class AArch64_CSel : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(AArch64_CSel, AllStages) { verifyAllStages(obj("test_csel_a64.o")); }

TEST_F(AArch64_CSel, NoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(obj("test_csel_a64.o"));
}

TEST_F(AArch64_CSel, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_csel_a64.o"));
}

TEST_F(AArch64_CSel, HasConditionalLogic) {
    verifyLLVMIRHasConditionalLogic(obj("test_csel_a64.o"));
}

TEST_F(AArch64_CSel, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(obj("test_csel_a64.o"));
}

TEST_F(AArch64_CSel, DecompileC) {
    verifyDecompileProducesOutput(obj("test_csel_a64.o"));
}

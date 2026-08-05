#include "NeverDLiftFixture.h"

class AArch64_MulDiv : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(AArch64_MulDiv, AllStages) { verifyAllStages(obj("test_muldiv_a64.o")); }

TEST_F(AArch64_MulDiv, NoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(obj("test_muldiv_a64.o"));
}

TEST_F(AArch64_MulDiv, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_muldiv_a64.o"));
}

TEST_F(AArch64_MulDiv, PreservesMulSemantic) {
    verifyLLVMIRContains(obj("test_muldiv_a64.o"), "test_mul_a64", "mul");
}

TEST_F(AArch64_MulDiv, PreservesDivSemantic) {
    verifyLLVMIRContains(obj("test_muldiv_a64.o"), "test_udiv_a64", "udiv");
}

TEST_F(AArch64_MulDiv, DecompileC) {
    verifyDecompileProducesOutput(obj("test_muldiv_a64.o"));
}

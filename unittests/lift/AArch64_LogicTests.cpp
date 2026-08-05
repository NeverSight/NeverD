#include "NeverDLiftFixture.h"

class AArch64_Logic : public NeverDLiftTest {};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

TEST_F(AArch64_Logic, AllStages) { verifyAllStages(obj("test_logic_a64.o")); }

TEST_F(AArch64_Logic, NoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(obj("test_logic_a64.o"));
}

TEST_F(AArch64_Logic, NoUnreachable) {
    verifyLLVMIRNoUnreachable(obj("test_logic_a64.o"));
}

TEST_F(AArch64_Logic, NoConstantTrueBranch) {
    verifyNoConstantTrueBranch(obj("test_logic_a64.o"));
}

TEST_F(AArch64_Logic, PreservesAndSemantic) {
    verifyLLVMIRContains(obj("test_logic_a64.o"), "test_and_a64", "and");
}

TEST_F(AArch64_Logic, PreservesOrSemantic) {
    verifyLLVMIRContains(obj("test_logic_a64.o"), "test_orr_a64", "or");
}

TEST_F(AArch64_Logic, PreservesXorSemantic) {
    verifyLLVMIRContains(obj("test_logic_a64.o"), "test_eor_a64", "xor");
}

TEST_F(AArch64_Logic, DecompileC) {
    verifyDecompileProducesOutput(obj("test_logic_a64.o"));
}

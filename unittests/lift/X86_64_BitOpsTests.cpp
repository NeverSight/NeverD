#include "NeverDLiftFixture.h"

class X86_64_BitOps : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_bitops.o";
}

TEST_F(X86_64_BitOps, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_bitops.o not built";
    verifyAllStages(testObj());
}

TEST_F(X86_64_BitOps, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_64_BitOps, BtHasIntAnd) {
    verifyLowIRContains(testObj(), "test_bt", "INT_AND");
}

TEST_F(X86_64_BitOps, BtsHasIntOr) {
    verifyLowIRContains(testObj(), "test_bts", "INT_OR");
}

TEST_F(X86_64_BitOps, BtrHasIntAnd) {
    verifyLowIRContains(testObj(), "test_btr", "INT_AND");
}

TEST_F(X86_64_BitOps, BtcHasIntXor) {
    verifyLowIRContains(testObj(), "test_btc", "INT_XOR");
}

TEST_F(X86_64_BitOps, BswapHasCopy) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_BitOps, PopcntPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_popcnt", "POPCOUNT");
}

TEST_F(X86_64_BitOps, LzcntPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_lzcnt", "LZCOUNT");
}

TEST_F(X86_64_BitOps, TzcntPreservesSemantics) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_FALSE(r.out.empty());
}

TEST_F(X86_64_BitOps, NoUnreachableInFunctions) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR:\n" << r.out;
}
